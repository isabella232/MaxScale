/**
 * @file connectionpool.c - Connection Pooling
 */

#include <connectionpool.h>
#include <atomic.h>
#include <dcb.h>
#include <log_manager.h>
#include <session.h>
#include <server.h>
#include <skygw_types.h>
#include <skygw_utils.h>
#include <spinlock.h>

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

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
