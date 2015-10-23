/**
 * @file connectionpool.c - Connection Pooling
 */

#include "connectionpool.h"
#include "atomic.h"
#include "dcb.h"
#include "log_manager.h"
#include "modutil.h"
#include "mysql_client_server_protocol.h"
#include "session.h"
#include "server.h"
#include "skygw_types.h"
#include "skygw_utils.h"
#include "spinlock.h"

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

/**
 * Airbnb connection proxy minutely stats structure. It uses 2 structures, one
 * for current minutely stats, and the other for last minutely. The minutely
 * rate metrics, e.g. queries per minute, are calculated by the difference of
 * the two minutely metric counters. Gauge metrics are always read from the
 * current minutely states structure.
 */
#define MINUTELY_LAST 0
#define MINUTELY_CURR 1
#define MINUTELY_SIZE 2
service_conn_pool_minutely_stats *conn_proxy_minutely = NULL;

int conn_proxy_stats_init_cb(SERVICE *service)
{
    if (conn_proxy_minutely != NULL)
        return 0;

    conn_proxy_minutely = (service_conn_pool_minutely_stats *)
        calloc(1, MINUTELY_SIZE * sizeof(service_conn_pool_minutely_stats));
    if (conn_proxy_minutely == NULL) {
        LOGIF(LE, (skygw_log_write_flush(
                     LOGFILE_ERROR,
                     "Error : Service %s failed to allocate minutely stats structure.",
                     service)));
        return 1;
    }
    return 0;
}

void conn_proxy_stats_close_cb(SERVICE *service)
{
    if (conn_proxy_minutely != NULL)
        free(conn_proxy_minutely);
    conn_proxy_minutely = NULL;
}

void
pool_init_queue_item(POOL_QUEUE_ITEM *queue_item, void *rses)
{
    queue_item->router_session = rses;
    queue_item->query_buf = NULL;
    queue_item->next = NULL;
}

/**
 * The helper function that returns the backend connection to its server
 * connection pool and unlinks its current client session. Because a pooling
 * connection returns to pool after it have forwarded response to client
 * session, it is called without having client's router session lock acquired.
 */
int
pool_park_connection(DCB *backend_dcb)
{
    bool rc = 0;
    SESSION *session = NULL;

    if (backend_dcb->state != DCB_STATE_POLLING) {
        return 0;
    }

    ss_dassert(backend_dcb->session != NULL);
    /* add backend DCB to server persistent connections pool */
    if (dcb_park_server_connection_pool(backend_dcb)) {
        backend_dcb->conn_pool_func->pool_link_cb(backend_dcb, 0, 0, NULL);
        rc = 1;
    }
    return rc;
}

/**
 * The helper function that looks for backend connection in the server
 * connection pool and links with the client session.
 *
 * @note It should be called with router session lock acquired.
 */
int
pool_unpark_connection(DCB **p_dcb, SESSION *client_session, SERVER *server,
		       char *user, void *cb_arg)
{
    DCB *dcb = NULL;

    ss_dassert(server != NULL && client_session != NULL);
    dcb = server_get_persistent(server, user, server->protocol);
    if (dcb == NULL)
        return 0;

    /* reset query response state before query routing */
    protocol_reset_query_response_state(dcb);

    LOGIF(LD, (skygw_log_write(
        LOGFILE_DEBUG,
        "%lu [pool_unpark_connection] pick up DCB %p for session %p query routing",
        pthread_self(), dcb, client_session)));

    /* link the backend connection with client session */
    if (!session_link_dcb(client_session, dcb)) {
        LOGIF(LD, (skygw_log_write(
            LOGFILE_DEBUG,
            "%lu [pool_unpark_connection] Failed to link to session %p, the "
            "session has been removed.\n",
            pthread_self(), client_session)));
        /* park the connection back in server pool */
        dcb_add_server_persistent_connection_fast(dcb);
        // FIXME(liang) distinguish disconnected client_session from no dcb avail
        return 0;
    }

    /* link backend DCB with router specific data structure */
    dcb->conn_pool_func->pool_link_cb(dcb, 1, 1, cb_arg);
    return 1;
}

int
server_backend_auth_connection_close_cb(DCB *backend_dcb)
{
    LOGIF(LD, (skygw_log_write(
        LOGFILE_DEBUG,
        "%lu [server_backend_auth_connection_close_cb] "
        "close connection auth DCB %p for server %p",
        pthread_self(), backend_dcb, backend_dcb->server)));
    session_unlink_dcb(backend_dcb->session, backend_dcb);
    /* unlink the backend dcb */
    backend_dcb->conn_pool_func->pool_link_cb(backend_dcb, 0, 0, NULL);
    dcb_close(backend_dcb);
    return 0;
}

/**
 * The function sniffs mysql packets for conclusion of query result set for
 * connection pooling backend connection DCB. It follows MySQL client server
 * protocol ProtocolText::Resultset for COM_QUERY_RESPONSE.
 *
 * A complete set of mysql packets for a normal query result is as follows,
 *
 * - column count packet
 * - N column definition packets, one packet for each column
 * - EOF packet
 * - R row data packets, one packet for each row
 * - EOF packet (or ERR packet), R is encoded in EOF packet
 *
 * For query that results in failure, it is one single ERR packet. For session
 * commands, it is one single OK packet.
 *
 * This function process a entire result set. It does not handle multi-resultset,
 * which is used by mysql stored procedure. But, it is easy to extend to support
 * multi-resultset.
 */
void
protocol_process_query_resultset(DCB *backend_dcb, GWBUF *response_buf, int first)
{
    CONN_POOL_QUERY_RESPONSE *resp = &backend_dcb->dcb_conn_pool_data.resp_state;
    unsigned char* buf_ptr = (unsigned char*)response_buf->start;
    unsigned char* buf_end = (unsigned char*)response_buf->end;

    /* check whether the first response packet is ERR or OK */
    if (first && (PTR_IS_ERR(buf_ptr) || PTR_IS_OK(buf_ptr))) {
        resp->resp_status = PTR_IS_ERR(buf_ptr) ? RESP_ERR : RESP_EOF;
        return;
    }

    /* scan column definitions packets in ProtocolText::Resultset */
    if (resp->resp_eof_count == 0) {
        int len;
        ss_dassert(resp->resp_status == RESP_NONE);
        for (len = 0; buf_ptr < buf_end; buf_ptr += len, resp->resp_ncols++) {
            len = MYSQL_GET_PACKET_LEN(buf_ptr) + 4;
            if (PTR_IS_ERR(buf_ptr) || PTR_IS_EOF(buf_ptr)) {
                resp->resp_status = PTR_IS_ERR(buf_ptr) ? RESP_ERR : RESP_NONE;
                resp->resp_eof_count++;
                /* move past the EOF packet */
                buf_ptr += len;
                /* discount the first columns count packet prior to column definition pakcets */
                resp->resp_ncols -= 1;
                break;
            }
        }
    }

    /* scan row data packets followed by the last EOF (or ERR) packet */
    if (resp->resp_status == RESP_NONE && resp->resp_eof_count == 1) {
        int len;
        for (len = 0; buf_ptr < buf_end; buf_ptr += len, resp->resp_nrows++) {
            len = MYSQL_GET_PACKET_LEN(buf_ptr) + 4;
            if (PTR_IS_ERR(buf_ptr) || PTR_IS_EOF(buf_ptr)) {
                resp->resp_status = PTR_IS_ERR(buf_ptr) ? RESP_ERR : RESP_EOF;
                resp->resp_eof_count++;
                /* move past the last EOF packet */
                buf_ptr += len;
                break;
            }
        }
        ss_dassert(buf_ptr == buf_end);
    }
}


/**
 * The housekeeper task collects minutely connection proxy internal stats for
 * router service and backend servers. It separates stats collection from stats
 * serving to external stats agent.
 */
static void
hktask_proxy_stats_minutely()
{
    service_conn_pool_minutely_stats *last = &conn_proxy_minutely[MINUTELY_LAST];
    service_conn_pool_minutely_stats *curr = &conn_proxy_minutely[MINUTELY_CURR];

    /* copy current minutely to last minutely before overwrite current stats */
    memcpy(last, curr, sizeof(service_conn_pool_minutely_stats));
    service_conn_pool_stats_minutely(curr);
}

void conn_proxy_stats_register_cb(SERVICE *service)
{
    hktask_add("connection_proxy_stats", hktask_proxy_stats_minutely, NULL, 60);
}

void
conn_proxy_export_stats_cb(struct dcb *dcb)
{
    service_conn_pool_minutely_stats *last = &conn_proxy_minutely[MINUTELY_LAST];
    service_conn_pool_minutely_stats *curr = &conn_proxy_minutely[MINUTELY_CURR];

    dcb_printf(dcb, "{\n");

    /* export servers stats */
    server_export_conn_pool_stats(dcb);

    dcb_printf(dcb, "\"proxy\": {\n");
    dcb_printf(dcb, "  \"queries_routed\": %d,\n",
               curr->n_queries_routed - last->n_queries_routed);
    dcb_printf(dcb, "  \"queries_to_master\": %d,\n",
               curr->n_queries_master - last->n_queries_master);
    dcb_printf(dcb, "  \"queries_to_slaves\": %d,\n",
               curr->n_queries_slave - last->n_queries_slave);
    dcb_printf(dcb, "  \"connection_reqs\": %d,\n",
               curr->n_conn_reqs - last->n_conn_reqs);
    dcb_printf(dcb, "  \"disconnection_reqs\": %d,\n",
               curr->n_disconn_reqs - last->n_disconn_reqs);
    dcb_printf(dcb, "  \"client_hangups\": %d,\n",
               curr->n_client_hangups - last->n_client_hangups);
    dcb_printf(dcb, "  \"client_errors\": %d,\n",
               curr->n_client_errors - last->n_client_errors);
    dcb_printf(dcb, "  \"client_sessions\": %d \n", curr->n_client_sessions);
    dcb_printf(dcb, "}\n");

    dcb_printf(dcb, "}\n");
}
