#ifndef _CONNECTIONPOOL_H
#define _CONNECTIONPOOL_H

#include <time.h>  // gettimeofday

/**
 * @file connectionpool.h - Airbnb connection proxy header file
 */

struct dcb;
struct session;
struct server;
struct server_connection_pool_queue_item;
struct service;
struct gwbuf;

typedef unsigned long long my_uint64;

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
    void (*router_proxy_export_stats)(struct dcb *);
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
  my_uint64 resp_bytes;         /* number of response bytes */
} CONN_POOL_QUERY_RESPONSE;


/**
 * Server level connection pooling stats
 */
struct server_conn_pool_stats {
    int n_pool_conns;              /* number of connections in pool */
    int n_parked_conns;            /* number of connections currently parking in pool */
    int n_queue_items;             /* number of waiting client router sessions */
    int n_conns_backend_errors;    /* number of connections backend errors */
    int n_parked_conns_errors;     /* number of parked connections backend errors */
    int n_conns_close_by_client_error; /* number of connections closed due to client session errors */
    int n_query_routing_errors;    /* number of query routing errors */
    int n_fast_resultset_proc;     /* number of optimized resultset processing */
    int n_normal_resultset_proc;   /* number of complete resultset processing */
    int n_throttled_queue_reqs;    /* number of throttled client requests */
};
typedef struct server_conn_pool_stats SERVER_CONN_POOL_STATS;


/**
 * Stats holder for server level minutely stats. It is used to export minutely stats
 * to external stats agent.
 */
struct server_conn_pool_minutely_stats {
    int n_conns_backend_errors;    /* number of connections backend errors */
    int n_parked_conns_errors;     /* number of parked connections backend errors */
    int n_conns_close_by_client_error; /* number of connections closed due to client session errors */
    int n_query_routing_errors;    /* number of query routing errors */
    int n_throttled_queue_reqs;    /* number of throttled client requests */
    int n_fast_resultset_proc;     /* number of optimized resultset processing */
    int n_normal_resultset_proc;   /* number of complete resultset processing */
};
typedef struct server_conn_pool_minutely_stats SERVER_CONN_POOL_MINUTELY_STATS;


/**
 * Service level connection pool counter stats
 */
struct service_conn_pool_stats {
    int n_conn_accepts;            /* number of connection requests */
    int n_client_sessions;         /* number of client connections */
    int n_client_disconnections;   /* number of client connection close events */
    int n_client_hangups;          /* number of client hangup events */
    int n_client_errors;           /* number of client error events */
    int n_client_full_cleanups;    /* number of full client sessions cleanup */
};
typedef struct service_conn_pool_stats SERVICE_CONN_POOL_STATS;


/**
 * Stats holder for Airbnb connection proxy service level minutely stats.
 */
struct service_conn_pool_minutely_stats {
    int n_queries_routed;          /* number of queries routed by the service */
    int n_queries_master;          /* number of queries routed to master backend */
    int n_queries_slave;           /* number of queries routed to slave backends */
    int n_conn_accepts;            /* number of client connection requests */
    int n_disconn_reqs;            /* number of client disconnection requests */
    int n_client_hangups;          /* number of client connection hangup events */
    int n_client_errors;           /* number of client connection error events */
    int n_client_full_cleanups;    /* number of full client sessions cleanup */
    int n_client_sessions;         /* number of current client sessions */
    my_uint64 queries_exec_time;   /* sum of all queries execution time within the period */
    my_uint64 query_max_exec_time; /* max query execution time within the period */
    my_uint64 query_min_exec_time; /* min query execution time within the period */
    my_uint64 mysql_exec_time;     /* sum of all mysql execution time within the period */
    my_uint64 mysql_max_exec_time; /* max mysql execution time within the period */
    my_uint64 mysql_min_exec_time; /* min mysql execution time within the period */
    my_uint64 response_size;       /* sum of all queries resultset size in bytes */
    my_uint64 response_max_size;   /* max query resultset size in bytes */
    my_uint64 response_min_size;   /* min query resultset size in bytes */
    int poll_events_queue_len;     /* epoll events queue length */
    int poll_events_queue_max;     /* minutely max events queue length */
};
typedef struct service_conn_pool_minutely_stats service_conn_pool_minutely_stats;


/** Client router session query state enumeration */
enum conn_pool_session_query_state {
    QUERY_IDLE = 0,
    QUERY_ROUTED,
    QUERY_RECEIVING_RESULT,
    QUERY_STATE_MAX
};


/** Router session connection pool management data */
struct session_conn_pool_data {
    enum conn_pool_session_query_state query_state; /* session query execution state */
    my_uint64 query_start;                          /* query execution start timer */
    my_uint64 query_exec_start;                     /* query execution start timer */
};
typedef struct session_conn_pool_data session_conn_pool_data;


/** Initialize server connection pool queue item */
void pool_init_queue_item(struct server_connection_pool_queue_item *queue_item,
			  void *router_ses);

/** Server connection pool helper functions */
int pool_park_connection(struct dcb *backend_dcb);
int pool_unpark_connection(struct dcb **p_dcb, struct session *client_session,
			   struct server *server, char *user, void *cb_arg);

/** Error handler for backend connection query routing failure */
void pool_handle_backend_failure(struct dcb *backend_dcb);

/**
 * Callbacks to initialize internal minutely conncection proxy stats structure
 * at service start and stop time.
 */
int conn_proxy_stats_init_cb(struct service *svc);
void conn_proxy_stats_close_cb(struct service *svc);
void conn_proxy_stats_register_cb(struct service *svc);

/**
 * The debugcmd callback function to export connection proxy internal stats to
 * external stats agent in JSON format.
 */
void conn_proxy_export_stats_cb(struct dcb *dcb);

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


/** The callback for backend connection not responding error condition */
void server_backend_connection_not_responding_cb(struct dcb *backend_dcb);

my_uint64 measure_query_elapsed_time_micros(my_uint64 query_start_micros,
                                            my_uint64 exec_start_micros);

/** Minutely task to collect epoll events stats for monitoring */
void poll_events_stats_minutely(service_conn_pool_minutely_stats *stats);

void track_query_resultset_stats(CONN_POOL_QUERY_RESPONSE *resp);


/**
 * Reset backend DCB connection pool query response state before routing query.
 */
#define protocol_reset_query_response_state(backend_dcb) \
  { CONN_POOL_QUERY_RESPONSE *resp = &backend_dcb->dcb_conn_pool_data.resp_state; \
    resp->resp_eof_count = resp->resp_ncols = 0; resp->resp_nrows = 0;            \
    resp->resp_more_result = 0;                                                   \
    resp->resp_status = RESP_NONE;                                                \
    resp->resp_bytes = 0;                                                         \
  }


/**
 * Check whether a backend connection has received complete resultset packets.
 */
#define CONN_POOL_DCB_RESULTSET_OK(dcb) \
  (dcb->dcb_conn_pool_data.resp_state.resp_status == RESP_EOF || \
   dcb->dcb_conn_pool_data.resp_state.resp_status == RESP_ERR)


/** Initialize server level connection pool stats */
#define server_init_conn_pool_stats(server) \
  {                                                          \
    server->conn_pool.pool_stats.n_pool_conns = 0;           \
    server->conn_pool.pool_stats.n_parked_conns = 0;         \
    server->conn_pool.pool_stats.n_queue_items = 0;          \
    server->conn_pool.pool_stats.n_conns_backend_errors = 0; \
    server->conn_pool.pool_stats.n_parked_conns_errors = 0;  \
    server->conn_pool.pool_stats.n_query_routing_errors = 0; \
    server->conn_pool.pool_stats.n_conns_close_by_client_error = 0; \
    server->conn_pool.pool_stats.n_fast_resultset_proc = 0;  \
    server->conn_pool.pool_stats.n_normal_resultset_proc = 0; \
    server->conn_pool.pool_stats.n_throttled_queue_reqs = 0; \
  }

/** Maintain minutely server level connection pool stats holder */
#define server_copy_minutely_conn_pool_stats(server) \
  {                                                                                 \
    SERVER_CONN_POOL_MINUTELY_STATS* last = &server->conn_pool.pool_stats_minutely; \
    last->n_conns_backend_errors = server->conn_pool.pool_stats.n_conns_backend_errors; \
    last->n_parked_conns_errors = server->conn_pool.pool_stats.n_parked_conns_errors; \
    last->n_query_routing_errors = server->conn_pool.pool_stats.n_query_routing_errors; \
    last->n_throttled_queue_reqs = server->conn_pool.pool_stats.n_throttled_queue_reqs; \
    last->n_fast_resultset_proc = server->conn_pool.pool_stats.n_fast_resultset_proc; \
    last->n_normal_resultset_proc = server->conn_pool.pool_stats.n_normal_resultset_proc; \
    last->n_conns_close_by_client_error = server->conn_pool.pool_stats.n_conns_close_by_client_error; \
    /* reset minutely counter stats */                                  \
    server->conn_pool.pool_stats.n_fast_resultset_proc = 0;             \
    server->conn_pool.pool_stats.n_normal_resultset_proc = 0;           \
  }

/** Initialize service level connection pool stats */
#define service_init_conn_pool_stats(service) \
 { SERVICE_CONN_POOL_STATS *stats = &service->conn_pool_stats; \
   stats->n_conn_accepts = 0;                                  \
   stats->n_client_sessions = 0;                               \
   stats->n_client_disconnections = 0;                         \
   stats->n_client_hangups = 0;                                \
   stats->n_client_errors = 0;                                 \
   stats->n_client_full_cleanups = 0;                          \
 }


/** Initialize router session connection pool state data */
#define session_init_conn_pool_data(router_ses) \
  { session_conn_pool_data *data = &router_ses->rses_conn_pool_data; \
    data->query_state = QUERY_IDLE;                                  \
    data->query_start = 0;                                           \
    data->query_exec_start = 0;                                      \
  }


/** Set timer to microseconds since epoch */
#define GET_TIMER_MICROS(timer) \
  {                                                                     \
    struct timeval tv;                                                  \
    my_uint64 tm_micros = 0;                                            \
    if (gettimeofday(&tv, NULL) == 0) {                                 \
      tm_micros = ((my_uint64)tv.tv_sec) * 1000000 + (my_uint64)tv.tv_usec; \
    }                                                                   \
    timer = tm_micros;                                                  \
  }


/** Start timer to measure query execution time within the Airbnb connection proxy */
#define START_ROUTER_SESSION_QUERY_TIMER(rses) \
  { GET_TIMER_MICROS(rses->rses_conn_pool_data.query_start); }


/** Start timer to measure mysql query response time at proxy side */
#define START_MYSQL_QUERY_EXEC_TIMER(rses) \
  { GET_TIMER_MICROS(rses->rses_conn_pool_data.query_exec_start); }

#endif /* _CONNECTIONPOOL_H */
