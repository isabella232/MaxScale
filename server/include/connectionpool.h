#ifndef _CONNECTIONPOOL_H
#define _CONNECTIONPOOL_H

/**
 * @file connectionpool.h - Airbnb connection proxy header file
 */

struct dcb;
struct session;
struct server;
struct server_connection_pool_queue_item;
struct service;
struct gwbuf;

/**
 * Connection pooling callback functions
 */
struct conn_pool_func {
    int	(*pool_cb)(struct dcb *);
    int (*pool_auth_cb)(struct dcb *);
    int (*pool_link_cb)(struct dcb *, int, int, void *);
};
typedef struct conn_pool_func CONN_POOL_FUNC;

/**
 * ROUTER_OBJECT connection pooling callback functions
 */
struct router_conn_pool_func {
    int  (*router_proxy_stats_init)(struct service *);
    void (*router_proxy_stats_close)(struct service *);
    void (*router_proxy_stats_register)(struct service *);
    void (*router_proxy_stats)(void *router_instance, void *stats);
};
typedef struct router_conn_pool_func ROUTER_CONN_POOL_FUNC;

#define RESP_NONE  0
#define RESP_EOF   1  /* EOF or OK packet */
#define RESP_ERR   2  /* ERR packet */

/**
 * Query response packets sniffing state. A pooling backend connection must
 * sniff query response packets and ensure receiving complete result set.
 * According to ProtocolText::Resultset, "resp_eof_count" should be 2 for
 * a complete query resultset, i.e. it has both column definitions part and
 * rows data part.
 */
typedef struct {
  int resp_eof_count;           /* number of EOF/ERR packets */
  int resp_ncols;               /* number of column packets */
  int resp_nrows;               /* number of row data packets */
  int resp_status;              /* response success 1, error 2 */
  int resp_more_result;         /* has multi resultset */
} CONN_POOL_QUERY_RESPONSE;


/**
 * Service level connection pool counter stats
 */
struct service_conn_pool_stats {
    int n_conn_accepts;            /* number of connection requests */
    int n_client_sessions;         /* number of client connections */
    int n_client_disconnections;   /* number of client connection close events */
    int n_client_hangups;          /* number of client hangup events */
    int n_client_errors;           /* number of client error events */
};
typedef struct service_conn_pool_stats SERVICE_CONN_POOL_STATS;


/**
 * Stats holder for Airbnb connection proxy service level minutely stats.
 */
struct service_conn_pool_minutely_stats {
    int n_queries_routed;          /* number of queries routed by the service */
    int n_queries_master;          /* number of queries routed to master backend */
    int n_queries_slave;           /* number of queries routed to slave backends */
    int n_conn_reqs;               /* number of client connection requests */
    int n_disconn_reqs;            /* number of client disconnection requests */
    int n_client_hangups;          /* number of client connection hangup events */
    int n_client_errors;           /* number of client connection error events */
    int n_client_sessions;         /* number of current client sessions */
};
typedef struct service_conn_pool_minutely_stats service_conn_pool_minutely_stats;


/** Client router session query state enumeration */
enum conn_pool_session_query_state {
    QUERY_IDLE = 0,
    QUERY_ROUTED,
    QUERY_RECEIVING_RESULT,
    QUERY_STATE_MAX
};


/** Initialize server connection pool queue item */
void pool_init_queue_item(struct server_connection_pool_queue_item *queue_item,
			  void *router_ses);

/** Server connection pool helper functions */
int pool_park_connection(struct dcb *backend_dcb);
int pool_unpark_connection(struct dcb **p_dcb, struct session *client_session,
			   struct server *server, char *user, void *cb_arg);

/**
 * Callbacks to initialize internal minutely conncection proxy stats structure
 * at service start and stop time.
 */
int conn_proxy_stats_init_cb(struct service *svc);
void conn_proxy_stats_close_cb(struct service *svc);
void conn_proxy_stats_register_cb(struct service *svc);

/**
 * The housekeeper task collects minutely connection proxy internal stats for
 * router service and backend servers. It separates stats collection from stats
 * serving to external stats agent.
 */
void hktask_proxy_stats_minutely();

/**
 * The callback is to terminate a server connection that had been used to complete
 * client connection authentication with backend server. A new client connection
 * always complete authentication with backend server, and it may create a backend
 * connection to MySQL server for that. Upon completion of authentication, it will
 * either park the connection in the pool or close it if the pool has been fully
 * bootstraped.
 */
int server_backend_auth_connection_close_cb(struct dcb *backend_dcb);

/**
 * This function inspects complete mysql packets in the query response data buffer
 * and checks whether the result set is complete for COM_QUERY_RESPONSE. The backend
 * DCB connection pool response state will track mysql packets received and resultset
 * status in the end.
 */
void protocol_process_query_resultset(struct dcb *backend_dcb, struct gwbuf *response,
                                      int first);


/**
 * Reset backend DCB connection pool query response state before routing query.
 */
#define protocol_reset_query_response_state(backend_dcb) \
  { CONN_POOL_QUERY_RESPONSE *resp = &backend_dcb->dcb_conn_pool_data.resp_state; \
    resp->resp_eof_count = resp->resp_ncols = 0; resp->resp_nrows = 0;            \
    resp->resp_more_result = 0;                                                   \
    resp->resp_status = RESP_NONE;                                                \
  }


/**
 * Check whether a backend connection has received complete resultset packets.
 */
#define CONN_POOL_DCB_RESULTSET_OK(dcb) \
  (dcb->dcb_conn_pool_data.resp_state.resp_status == RESP_EOF || \
   dcb->dcb_conn_pool_data.resp_state.resp_status == RESP_ERR)


/** Initialize service level connection pool stats */
#define service_init_conn_pool_stats(service) \
 { SERVICE_CONN_POOL_STATS *stats = &service->conn_pool_stats; \
   stats->n_conn_accepts = 0;                                  \
   stats->n_client_sessions = 0;                               \
   stats->n_client_disconnections = 0;                         \
   stats->n_client_hangups = 0;                                \
   stats->n_client_errors = 0;                                 \
 }

#endif /* _CONNECTIONPOOL_H */
