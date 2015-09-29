#ifndef _CONNECTIONPOOL_H
#define _CONNECTIONPOOL_H

/**
 * @file connectionpool.h - Airbnb connection proxy header file
 */

struct dcb;
struct session;
struct server;
struct server_connection_pool_queue_item;

/**
 * Connection pooling callback functions
 */
struct conn_pool_func {
    int	(*pool_cb)(struct dcb *);
    int (*pool_auth_cb)(struct dcb *);
    int (*pool_link_cb)(struct dcb *, int, int, void *);
};
typedef struct conn_pool_func CONN_POOL_FUNC;

void pool_init_queue_item(struct server_connection_pool_queue_item *queue_item,
			  void *router_ses);

int pool_park_connection(struct dcb *backend_dcb);
int pool_unpark_connection(struct dcb **p_dcb, struct session *client_session,
			   struct server *server, char *user, void *cb_arg);

/**
 * The callback is to terminate a server connection that had been used to complete
 * client connection authentication with backend server. A new client connection
 * always complete authentication with backend server, and it may create a backend
 * connection to MySQL server for that. Upon completion of authentication, it will
 * either park the connection in the pool or close it if the pool has been fully
 * bootstraped.
 */
int server_backend_auth_connection_close_cb(struct dcb *backend_dcb);

#endif /* _CONNECTIONPOOL_H */
