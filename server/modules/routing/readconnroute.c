/*
 * This file is distributed as part of the MariaDB Corporation MaxScale.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright MariaDB Corporation Ab 2013-2014
 */

/**
 * @file readconnroute.c - Read Connection Load Balancing Query Router
 *
 * This is the implementation of a simple query router that balances
 * read connections. It assumes the service is configured with a set
 * of slaves and that the application clients already split read and write
 * queries. It offers a service to balance the client read connections
 * over this set of slave servers. It does this once only, at the time
 * the connection is made. It chooses the server that currently has the least
 * number of connections by keeping a count for each server of how
 * many connections the query router has made to the server.
 *
 * When two servers have the same number of current connections the one with
 * the least number of connections since startup will be used.
 *
 * The router may also have options associated to it that will limit the
 * choice of backend server. Currently two options are supported, the "master"
 * option will cause the router to only connect to servers marked as masters
 * and the "slave" option will limit connections to routers that are marked
 * as slaves. If neither option is specified the router will connect to either
 * masters or slaves.
 *
 * @verbatim
 * Revision History
 *
 * Date		Who		Description
 * 14/06/2013	Mark Riddoch		Initial implementation
 * 25/06/2013	Mark Riddoch		Addition of checks for current server state
 * 26/06/2013	Mark Riddoch		Use server with least connections since
 * 					startup if the number of current
 * 					connections is the same for two servers
 * 					Addition of master and slave options
 * 27/06/2013	Vilho Raatikka		Added skygw_log_write command as an example
 *					and necessary headers.
 * 17/07/2013	Massimiliano Pinto	Added clientReply routine:
 *					called by backend server to send data to client
 *					Included mysql_client_server_protocol.h
 *					with macros and MySQL commands with MYSQL_ prefix
 *					avoiding any conflict with the standard ones
 *					in mysql.h
 * 22/07/2013	Mark Riddoch		Addition of joined router option for Galera
 * 					clusters
 * 31/07/2013	Massimiliano Pinto	Added a check for candidate server, if NULL return
 * 12/08/2013	Mark Riddoch		Log unsupported router options
 * 04/09/2013	Massimiliano Pinto	Added client NULL check in clientReply
 * 22/10/2013	Massimiliano Pinto	errorReply called from backend, for client error reply
 *					or take different actions such as open a new backend connection
 * 20/02/2014	Massimiliano Pinto	If router_options=slave, route traffic to master if no slaves available
 * 06/03/2014	Massimiliano Pinto	Server connection counter is now updated in closeSession
 * 24/06/2014	Massimiliano Pinto	New rules for selecting the Master server
 * 27/06/2014	Mark Riddoch		Addition of server weighting
 * 11/06/2015   Martin Brampton     Remove decrement n_current (moved to dcb.c)
 *
 * @endverbatim
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <service.h>
#include <server.h>
#include <router.h>
#include <atomic.h>
#include <spinlock.h>
#include <readconnection.h>
#include <dcb.h>
#include <spinlock.h>
#include <modinfo.h>

#include <skygw_types.h>
#include <skygw_utils.h>
#include <log_manager.h>

#include <mysql_client_server_protocol.h>

#include "modutil.h"

#include "connectionpool.h"

/** Defined in log_manager.cc */
extern int            lm_enabled_logfiles_bitmask;
extern size_t         log_ses_count[];
extern __thread log_info_t tls_log_info;

MODULE_INFO 	info = {
	MODULE_API_ROUTER,
	MODULE_GA,
	ROUTER_VERSION,
	"A connection based router to load balance based on connections"
};

static char *version_str = "V1.1.0";

/* The router entry points */
static	ROUTER	*createInstance(SERVICE *service, char **options);
static	void	*newSession(ROUTER *instance, SESSION *session);
static	void 	closeSession(ROUTER *instance, void *router_session);
static	void 	freeSession(ROUTER *instance, void *router_session);
static	int	routeQuery(ROUTER *instance, void *router_session, GWBUF *queue);
static	void	diagnostics(ROUTER *instance, DCB *dcb);
static  void    clientReply(
        ROUTER  *instance,
        void    *router_session,
        GWBUF   *queue,
        DCB     *backend_dcb);
static  void             handleError(
        ROUTER           *instance,
        void             *router_session,
        GWBUF            *errbuf,
        DCB              *backend_dcb,
        error_action_t   action,
        bool             *succp);
static  uint8_t getCapabilities (ROUTER* inst, void* router_session);


/** The module object definition */
static ROUTER_OBJECT MyObject = {
    createInstance,
    newSession,
    closeSession,
    freeSession,
    routeQuery,
    diagnostics,
    clientReply,
    handleError,
    getCapabilities
};

static bool rses_begin_locked_router_action(
        ROUTER_CLIENT_SES* rses);

static void rses_end_locked_router_action(
        ROUTER_CLIENT_SES* rses);

static BACKEND *get_root_master(
	BACKEND **servers);
static int handle_state_switch(
    DCB* dcb,DCB_REASON reason, void * routersession);
static SPINLOCK	instlock;
static ROUTER_INSTANCE *instances;

static void init_connection_pool_dcb(DCB *backend_dcb, ROUTER_CLIENT_SES *rses,
                                     ROUTER_INSTANCE *inst);
static void dequeue_server_connection_pool(ROUTER_CLIENT_SES *rses);
static int try_server_connection_or_enqueue(DCB **p_dcb, ROUTER_CLIENT_SES *rses,
					    GWBUF *querybuf);
static int server_backend_connection_pool_cb(DCB *backend_dcb);
static int server_backend_connection_pool_link_cb(DCB *backend_dcb, int link_mode,
						  int rses_locked, void *arg);
static void router_conn_pool_stats_cb(void *router_instance, void *stats_arg);

/* Connection pooling callback functions */
static CONN_POOL_FUNC conn_pool_cb = {
    server_backend_connection_pool_cb,
    server_backend_auth_connection_close_cb,
    server_backend_connection_pool_link_cb
};

static ROUTER_CONN_POOL_FUNC router_conn_pool_cb = {
  conn_proxy_stats_init_cb,
  conn_proxy_stats_close_cb,
  conn_proxy_stats_register_cb,
  router_conn_pool_stats_cb,
  conn_proxy_export_stats_cb
};

/**
 * Implementation of the mandatory version entry point
 *
 * @return version string of the module
 */
char *
version()
{
	return version_str;
}

/**
 * The module initialisation routine, called when the module
 * is first loaded.
 */
void
ModuleInit()
{
        LOGIF(LM, (skygw_log_write(
                           LOGFILE_MESSAGE,
                           "Initialise readconnroute router module %s.\n", version_str)));
        spinlock_init(&instlock);
	instances = NULL;
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
ROUTER_OBJECT *
GetModuleObject()
{
	return &MyObject;
}

/**
 * Create an instance of the router for a particular service
 * within the gateway.
 * 
 * @param service	The service this router is being create for
 * @param options	An array of options for this query router
 *
 * @return The instance data for this new instance
 */
static	ROUTER	*
createInstance(SERVICE *service, char **options)
{
ROUTER_INSTANCE	*inst;
SERVER		*server;
SERVER_REF      *sref;
int		i, n;
BACKEND		*backend;
char		*weightby;

        if ((inst = calloc(1, sizeof(ROUTER_INSTANCE))) == NULL) {
                return NULL;
        }

	inst->service = service;
	spinlock_init(&inst->lock);

	/*
	 * We need an array of the backend servers in the instance structure so
	 * that we can maintain a count of the number of connections to each
	 * backend server.
	 */
	for (sref = service->dbref, n = 0; sref; sref = sref->next)
		n++;

	inst->servers = (BACKEND **)calloc(n + 1, sizeof(BACKEND *));
	if (!inst->servers)
	{
		free(inst);
		return NULL;
	}

	for (sref = service->dbref, n = 0; sref; sref = sref->next)
	{
		if ((inst->servers[n] = malloc(sizeof(BACKEND))) == NULL)
		{
			for (i = 0; i < n; i++)
				free(inst->servers[i]);
			free(inst->servers);
			free(inst);
			return NULL;
		}
		inst->servers[n]->server = sref->server;
		inst->servers[n]->current_connection_count = 0;
		inst->servers[n]->weight = 1000;
		n++;
	}
	inst->servers[n] = NULL;

	/* Airproxy lazily sets router connection pooling callback pointer */
	if (service->conn_pool_func == NULL) {
	    service->conn_pool_func = &router_conn_pool_cb;
	}

	if ((weightby = serviceGetWeightingParameter(service)) != NULL)
	{
		int total = 0;
		for (n = 0; inst->servers[n]; n++)
		{
			backend = inst->servers[n];
			total += atoi(serverGetParameter(backend->server,
						weightby));
		}
		if (total == 0)
		{
			LOGIF(LE, (skygw_log_write(LOGFILE_ERROR,
				"WARNING: Weighting Parameter for service '%s' "
				"will be ignored as no servers have values "
				"for the parameter '%s'.\n",
				service->name, weightby)));
		}
		else
		{
			for (n = 0; inst->servers[n]; n++)
			{
				int perc, wght;
				backend = inst->servers[n];
				perc = ((wght = atoi(serverGetParameter(backend->server,
						weightby))) * 1000) / total;
				if (perc == 0 && wght != 0)
					perc = 1;
				backend->weight = perc;
				if (perc == 0)
				{
					LOGIF(LE, (skygw_log_write(
							LOGFILE_ERROR,
						"Server '%s' has no value "
						"for weighting parameter '%s', "
						"no queries will be routed to "
						"this server.\n",
						inst->servers[n]->server->unique_name,
						weightby)));
				}
		
			}
		}
	}

	/*
	 * Process the options
	 */
	inst->bitmask = 0;
	inst->bitvalue = 0;
	if (options)
	{
		for (i = 0; options[i]; i++)
		{
			if (!strcasecmp(options[i], "master"))
			{
				inst->bitmask |= (SERVER_MASTER|SERVER_SLAVE);
				inst->bitvalue |= SERVER_MASTER;
			}
			else if (!strcasecmp(options[i], "slave"))
			{
				inst->bitmask |= (SERVER_MASTER|SERVER_SLAVE);
				inst->bitvalue |= SERVER_SLAVE;
			}
			else if (!strcasecmp(options[i], "running"))
			{
				inst->bitmask |= (SERVER_RUNNING);
				inst->bitvalue |= SERVER_RUNNING;
			}
			else if (!strcasecmp(options[i], "synced"))
			{
				inst->bitmask |= (SERVER_JOINED);
				inst->bitvalue |= SERVER_JOINED;
			}
			else if (!strcasecmp(options[i], "ndb"))
			{
				inst->bitmask |= (SERVER_NDB);
				inst->bitvalue |= SERVER_NDB;
			}
			else
			{
                            LOGIF(LM, (skygw_log_write(
                                          LOGFILE_MESSAGE,
                                           "* Warning : Unsupported router "
                                           "option \'%s\' for readconnroute. "
                                           "Expected router options are "
                                           "[slave|master|synced|ndb]",
                                               options[i])));
			}
		}
	}
	if(inst->bitmask == 0 && inst->bitvalue == 0)
	{
	    /** No parameters given, use RUNNING as a valid server */
	    inst->bitmask |= (SERVER_RUNNING);
	    inst->bitvalue |= SERVER_RUNNING;
	}
	/*
	 * We have completed the creation of the instance data, so now
	 * insert this router instance into the linked list of routers
	 * that have been created with this module.
	 */
	spinlock_acquire(&instlock);
	inst->next = instances;
	instances = inst;
	spinlock_release(&instlock);

	return (ROUTER *)inst;
}

/**
 * Associate a new session with this instance of the router.
 *
 * @param instance	The router instance data
 * @param session	The session itself
 * @return Session specific data for this session
 */
static	void	*
newSession(ROUTER *instance, SESSION *session)
{
ROUTER_INSTANCE	        *inst = (ROUTER_INSTANCE *)instance;
ROUTER_CLIENT_SES       *client_rses;
BACKEND                 *candidate = NULL;
int                     i;
BACKEND *master_host = NULL;

        /* Airproxy restricts to one single user for connection pool, and reject other
         * users except service health check user */
#if 0
        if (config_connection_pool_enabled())
        {
            char *user = session_getUser(session);
            if (strcmp(user, config_service_health_check_user()) != 0 &&
                strcmp(user, config_server_connection_pool_user()) != 0)
            {
                return NULL;
            }
        }
#endif

        LOGIF(LD, (skygw_log_write_flush(
                LOGFILE_DEBUG,
                "%lu [newSession] new router session with session "
                "%p, and inst %p.",
                pthread_self(),
                session,
                inst)));


	client_rses = (ROUTER_CLIENT_SES *)calloc(1, sizeof(ROUTER_CLIENT_SES));

        if (client_rses == NULL) {
                return NULL;
	}

#if defined(SS_DEBUG)
        client_rses->rses_chk_top = CHK_NUM_ROUTER_SES;
        client_rses->rses_chk_tail = CHK_NUM_ROUTER_SES;
#endif

	/**
         * Find the Master host from available servers
	 */
        master_host = get_root_master(inst->servers);

	/**
	 * Find a backend server to connect to. This is the extent of the
	 * load balancing algorithm we need to implement for this simple
	 * connection router.
	 */

	/*
	 * Loop over all the servers and find any that have fewer connections
         * than the candidate server.
	 *
	 * If a server has less connections than the current candidate we mark this
	 * as the new candidate to connect to.
	 *
	 * If a server has the same number of connections currently as the candidate
	 * and has had less connections over time than the candidate it will also
	 * become the new candidate. This has the effect of spreading the
         * connections over different servers during periods of very low load.
	 */
	for (i = 0; inst->servers[i]; i++) {
		if(inst->servers[i]) {
			LOGIF(LD, (skygw_log_write(
				LOGFILE_DEBUG,
				"%lu [newSession] Examine server in port %d with "
                                "%d connections. Status is %s, "
				"inst->bitvalue is %d",
                                pthread_self(),
				inst->servers[i]->server->port,
				inst->servers[i]->current_connection_count,
				STRSRVSTATUS(inst->servers[i]->server),
				inst->bitmask)));
		}

		if (SERVER_IN_MAINT(inst->servers[i]->server))
			continue;

		if (inst->servers[i]->weight == 0)
			continue;

		/* Check server status bits against bitvalue from router_options */
		if (inst->servers[i] &&
			SERVER_IS_RUNNING(inst->servers[i]->server) &&
			(inst->servers[i]->server->status & inst->bitmask & inst->bitvalue))
                {
			if (master_host) {
				if (inst->servers[i] == master_host && (inst->bitvalue & SERVER_SLAVE)) {
					/* skip root Master here, as it could also be slave of an external server
					 * that is not in the configuration.
					 * Intermediate masters (Relay Servers) are also slave and will be selected
					 * as Slave(s)
			 		 */

					continue;
				}
				if (inst->servers[i] == master_host && (inst->bitvalue & SERVER_MASTER)) {
					/* If option is "master" return only the root Master as there
					 * could be intermediate masters (Relay Servers)
					 * and they must not be selected.
			 	 	 */

					candidate = master_host;
					break;
				}
			} else {
					/* master_host is NULL, no master server.
					 * If requested router_option is 'master'
					 * candidate wll be NULL.
					 */
					if (inst->bitvalue & SERVER_MASTER) {
                                                candidate = NULL;
                                                break;
					}
			}

			/* If no candidate set, set first running server as
			our initial candidate server */
			if (candidate == NULL)
                        {
				candidate = inst->servers[i];
			}
                        else if ((inst->servers[i]->current_connection_count 
					* 1000) / inst->servers[i]->weight <
                                   (candidate->current_connection_count *
					1000) / candidate->weight)
                        {
				/* This running server has fewer
				connections, set it as a new candidate */
				candidate = inst->servers[i];
			}
                        else if ((inst->servers[i]->current_connection_count 
					* 1000) / inst->servers[i]->weight ==
                                   (candidate->current_connection_count *
					1000) / candidate->weight &&
                                 inst->servers[i]->server->stats.n_connections <
                                 candidate->server->stats.n_connections)
                        {
				/* This running server has the same number
				of connections currently as the candidate
				but has had fewer connections over time
				than candidate, set this server to candidate*/
				candidate = inst->servers[i];
			}
		}
	}

	/* There is no candidate server here!
	 * With router_option=slave a master_host could be set, so route traffic there.
	 * Otherwise, just clean up and return NULL
	 */
	if (!candidate) {
		if (master_host) {
			candidate = master_host;
		} else {
                	LOGIF(LE, (skygw_log_write_flush(
                      	  LOGFILE_ERROR,
                      	  "Error : Failed to create new routing session. "
                      	  "Couldn't find eligible candidate server. Freeing "
                       	 "allocated resources.")));
			free(client_rses);
			return NULL;
		}
	}

	client_rses->rses_capabilities = RCAP_TYPE_PACKET_INPUT;

	/* initialize embedded queue item */
	pool_init_queue_item(&client_rses->rses_queue_item, client_rses);
	session_init_conn_pool_data(client_rses);
        
	/*
	 * We now have the server with the least connections.
	 * Bump the connection count for this server
	 */
	atomic_add(&candidate->current_connection_count, 1);
	client_rses->backend = candidate;
        LOGIF(LD, (skygw_log_write(
                LOGFILE_DEBUG,
                "%lu [newSession] Selected server in port %d. "
                "Connections : %d\n",
                pthread_self(),
                candidate->server->port,
                candidate->current_connection_count)));

        /*
	 * Open a backend connection, putting the DCB for this
	 * connection in the client_rses->backend_dcb
	 */
        client_rses->backend_dcb = dcb_connect(candidate->server,
                                      session,
                                      candidate->server->protocol);
        if (client_rses->backend_dcb == NULL)
	{
                atomic_add(&candidate->current_connection_count, -1);
		free(client_rses);
		return NULL;
	}
        dcb_add_callback(
                         client_rses->backend_dcb,
                         DCB_REASON_NOT_RESPONDING,
                         &handle_state_switch,
                         client_rses);
        inst->stats.n_sessions++;

	/* set up backend connection DCB with pooling callback functors */
	init_connection_pool_dcb(client_rses->backend_dcb, client_rses, inst);

        /* Airproxy maintains service connection pool stats */
        if (inst->service != NULL) {
            atomic_add(&inst->service->conn_pool_stats.n_client_sessions, 1);
        }

	/**
         * Add this session to the list of active sessions.
         */
	spinlock_acquire(&inst->lock);
	client_rses->next = inst->connections;
	inst->connections = client_rses;
	spinlock_release(&inst->lock);

        CHK_CLIENT_RSES(client_rses);

	skygw_log_write(
                LOGFILE_TRACE,
		 "Readconnroute: New session for server %s. "
                "Connections : %d",
		 candidate->server->unique_name,
		 candidate->current_connection_count);
	return (void *)client_rses;
}

/**
 * @node Unlink from backend server, unlink from router's connection list,
 * and free memory of a router client session.  
 *
 * Parameters:
 * @param router - <usage>
 *          <description>
 *
 * @param router_cli_ses - <usage>
 *          <description>
 *
 * @return void
 *
 * 
 * @details (write detailed description here)
 *
 */
static void freeSession(
        ROUTER* router_instance,
        void*   router_client_ses)
{
        ROUTER_INSTANCE*   router = (ROUTER_INSTANCE *)router_instance;
        ROUTER_CLIENT_SES* router_cli_ses =
                (ROUTER_CLIENT_SES *)router_client_ses;
        int prev_val;
        
        prev_val = atomic_add(&router_cli_ses->backend->current_connection_count, -1);
        ss_dassert(prev_val > 0);
        
	spinlock_acquire(&router->lock);
        
	if (router->connections == router_cli_ses) {
		router->connections = router_cli_ses->next;
        } else {
		ROUTER_CLIENT_SES *ptr = router->connections;
                
		while (ptr != NULL && ptr->next != router_cli_ses) {
			ptr = ptr->next;
                }
                
		if (ptr != NULL) {
			ptr->next = router_cli_ses->next;
                }
	}
	spinlock_release(&router->lock);

        LOGIF(LD, (skygw_log_write_flush(
                LOGFILE_DEBUG,
                "%lu [freeSession] Unlinked router_client_session %p from "
                "router %p and from server on port %d. Connections : %d. ",
                pthread_self(),
                router_cli_ses,
                router,
                router_cli_ses->backend->server->port,
                prev_val-1)));

        free(router_cli_ses);
}


/**
 * Close a session with the router, this is the mechanism
 * by which a router may cleanup data structure etc.
 *
 * @param instance		The router instance data
 * @param router_session	The session being closed
 */
static	void 	
closeSession(ROUTER *instance, void *router_session)
{
ROUTER_CLIENT_SES *router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
DCB*              backend_dcb;

        CHK_CLIENT_RSES(router_cli_ses);
        /**
         * Lock router client session for secure read and update.
         */
        if (rses_begin_locked_router_action(router_cli_ses))
        {
		/* decrease server current connection counter */

                backend_dcb = router_cli_ses->backend_dcb;
                router_cli_ses->backend_dcb = NULL;
                router_cli_ses->rses_closed = true;
                /* Airproxy clean up server connection pool queue request */
                if (router_cli_ses->rses_queue_item.query_buf != NULL) {
                    dequeue_server_connection_pool(router_cli_ses);
                }
                /* Airproxy maintains service connection pool stats */
                if (router_cli_ses->rses_router != NULL &&
                    router_cli_ses->rses_router->service != NULL)
                {
                    atomic_add(&router_cli_ses->rses_router->service->conn_pool_stats.n_client_sessions, -1);
                }
                /** Unlock */
                rses_end_locked_router_action(router_cli_ses);
                
                /**
                 * Close the backend server connection
                 */
                if (backend_dcb != NULL) {
                        CHK_DCB(backend_dcb);
                        dcb_close(backend_dcb);
                }
        }
}

/**
 * We have data from the client, we must route it to the backend.
 * This is simply a case of sending it to the connection that was
 * chosen when we started the client session.
 *
 * @param instance		The router instance
 * @param router_session	The router session returned from the newSession call
 * @param queue			The queue of data buffers to route
 * @return if succeed 1, otherwise 0
 */
static	int	
routeQuery(ROUTER *instance, void *router_session, GWBUF *queue)
{
        ROUTER_INSTANCE	  *inst = (ROUTER_INSTANCE *)instance;
        ROUTER_CLIENT_SES *router_cli_ses = (ROUTER_CLIENT_SES *)router_session;
        uint8_t           *payload = GWBUF_DATA(queue);
        int               mysql_command;
        int               rc;
        DCB*              backend_dcb;
        bool              rses_is_closed;
       
	inst->stats.n_queries++;
	mysql_command = MYSQL_GET_COMMAND(payload);

        /** Dirty read for quick check if router is closed. */
        if (router_cli_ses->rses_closed)
        {
                rses_is_closed = true;
        }
        else
        {
                /**
                 * Lock router client session for secure read of DCBs
                 */
                rses_is_closed = !(rses_begin_locked_router_action(router_cli_ses));
        }

        if (!rses_is_closed)
        {
                /* Airproxy starts measuring query execution time */
                START_ROUTER_SESSION_QUERY_TIMER(router_cli_ses);

                backend_dcb = router_cli_ses->backend_dcb;           
                /** unlock */
                rses_end_locked_router_action(router_cli_ses);
        }

        /* Airproxy checks for available backend connection in pool */
        if (config_connection_pool_enabled() && backend_dcb == NULL)
        {
	    if (!(rses_is_closed || SERVER_IS_DOWN(router_cli_ses->backend->server)))
            {
                int ret = try_server_connection_or_enqueue(&backend_dcb, router_cli_ses, queue);
                /* -1 means serious condition e.g. client session was gone */
                rc = (ret != -1) ? 1 : 0;
                /* found backend connection in pool */
                if (ret == 0) {
	            ss_dassert(backend_dcb != NULL);
                } else {
                    goto return_rc;
                }
            }
        }

        if (rses_is_closed ||  backend_dcb == NULL ||
            SERVER_IS_DOWN(router_cli_ses->backend->server))
        {
                LOGIF(LT, (skygw_log_write(
                        LOGFILE_TRACE|LOGFILE_ERROR,
                        "Error : Failed to route MySQL command %d to backend "
                        "server.%s",
                        mysql_command,rses_is_closed ? " Session is closed." : "")));
		rc = 0;
                goto return_rc;

        }

	char* trc = NULL;

        switch(mysql_command) {
		case MYSQL_COM_CHANGE_USER:
			rc = backend_dcb->func.auth(
				backend_dcb,
				NULL,
				backend_dcb->session,
				queue);
			break;
		case MYSQL_COM_QUERY:
			LOGIF(LOGFILE_TRACE,(trc = modutil_get_SQL(queue)));
		default:
			rc = backend_dcb->func.write(backend_dcb, queue);
                        /* Airproxy tracks router client session query state */
			if (rc == 1 &&
			    SERVER_USE_CONN_POOL(backend_dcb->server) &&
			    DCB_IS_IN_CONN_POOL(backend_dcb))
			{
			    router_cli_ses->rses_conn_pool_data.query_state = QUERY_ROUTED;
			    START_MYSQL_QUERY_EXEC_TIMER(router_cli_ses);
			}
			break;
        }

	LOGIF(LOGFILE_TRACE,skygw_log_write(
                LOGFILE_DEBUG|LOGFILE_TRACE,
		 "Routed [%s] to '%s'%s%s",
		 STRPACKETTYPE(mysql_command),
		 backend_dcb->server->unique_name,
		 trc?": ":".",
		 trc?trc:""));
	free(trc);
return_rc:
        return rc;
}

/**
 * Display router diagnostics
 *
 * @param instance	Instance of the router
 * @param dcb		DCB to send diagnostics to
 */
static	void
diagnostics(ROUTER *router, DCB *dcb)
{
ROUTER_INSTANCE	  *router_inst = (ROUTER_INSTANCE *)router;
ROUTER_CLIENT_SES *session;
int		  i = 0;
BACKEND		  *backend;
char              *weightby;

	spinlock_acquire(&router_inst->lock);
	session = router_inst->connections;
	while (session)
	{
		i++;
		session = session->next;
	}
	spinlock_release(&router_inst->lock);
	
	dcb_printf(dcb, "\tNumber of router sessions:   	%d\n",
                   router_inst->stats.n_sessions);
	dcb_printf(dcb, "\tCurrent no. of router sessions:	%d\n", i);
	dcb_printf(dcb, "\tNumber of queries forwarded:   	%d\n",
                   router_inst->stats.n_queries);
	if ((weightby = serviceGetWeightingParameter(router_inst->service))
							!= NULL)
	{
		dcb_printf(dcb, "\tConnection distribution based on %s "
				"server parameter.\n",
				weightby);
		dcb_printf(dcb,
			"\t\tServer               Target %% Connections\n");
		for (i = 0; router_inst->servers[i]; i++)
		{
			backend = router_inst->servers[i];
			dcb_printf(dcb, "\t\t%-20s %3.1f%%     %d\n",
				backend->server->unique_name,
				(float)backend->weight / 10,
				backend->current_connection_count);
		}
		
	}
}

/**
 * Client Reply routine
 *
 * The routine will reply to client data from backend server
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       backend_dcb     The backend DCB
 * @param       queue           The GWBUF with reply data
 */
static  void
clientReply(
        ROUTER *instance,
        void   *router_session,
        GWBUF  *queue,
        DCB    *backend_dcb)
{
	ROUTER_CLIENT_SES* router_cli_ses = (ROUTER_CLIENT_SES*) router_session;
	if (!rses_begin_locked_router_action(router_cli_ses)) {
	    /* consume buffer so that it will be freed */
	    GWBUF *buf = queue;
	    while ((buf = gwbuf_consume(buf, GWBUF_LENGTH(buf))) != NULL);
	    return;
	}

	if (router_cli_ses->rses_conn_pool_data.query_state != QUERY_RECEIVING_RESULT) {
	    ss_dassert(router_cli_ses->rses_conn_pool_data.query_state == QUERY_ROUTED ||
	               (router_cli_ses->rses_conn_pool_data.query_state == QUERY_IDLE &&
	                DCB_IS_IN_AUTH_PHASE(backend_dcb)));
	    /* Airproxy track router client session query state */
	    protocol_reset_query_response_state(backend_dcb);
	    router_cli_ses->rses_conn_pool_data.query_state = QUERY_RECEIVING_RESULT;
	    /* Airproxy process resultset packets */
	    protocol_process_query_resultset(backend_dcb, queue, 1);
	}
	/* Airproxy continue scanning mysql packets for partial query resultset */
	else {
	    protocol_process_query_resultset(backend_dcb, queue, 0);
	}

	ss_dassert(backend_dcb->session->client != NULL);
	SESSION_ROUTE_REPLY(backend_dcb->session, queue);

	/* Airproxy track router client session query state */
	if (CONN_POOL_DCB_RESULTSET_OK(backend_dcb)) {
	    router_cli_ses->rses_conn_pool_data.query_state = QUERY_IDLE;
	}

	rses_end_locked_router_action(router_cli_ses);
}

/**
 * Error Handler routine
 *
 * The routine will handle errors that occurred in backend writes.
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       message         The error message to reply
 * @param       backend_dcb     The backend DCB
 * @param       action     	The action: REPLY, REPLY_AND_CLOSE, NEW_CONNECTION
 *
 */
static void handleError(
	ROUTER           *instance,
	void             *router_session,
	GWBUF            *errbuf,
	DCB              *backend_dcb,
	error_action_t   action,
	bool             *succp)

{
	DCB             *client_dcb;
	SESSION         *session = backend_dcb->session;
	session_state_t sesstate;

	/** Reset error handle flag from a given DCB */
	if (action == ERRACT_RESET)
	{
		backend_dcb->dcb_errhandle_called = false;
		return;
	}
	
	/** Don't handle same error twice on same DCB */
	if (backend_dcb->dcb_errhandle_called)
	{
		/** we optimistically assume that previous call succeed */
		*succp = true;
		return;
	}
	else
	{
		backend_dcb->dcb_errhandle_called = true;
	}
	spinlock_acquire(&session->ses_lock);
	sesstate = session->state;
	client_dcb = session->client;
	
	if (sesstate == SESSION_STATE_ROUTER_READY)
	{
		CHK_DCB(client_dcb);
		spinlock_release(&session->ses_lock);	
		client_dcb->func.write(client_dcb, gwbuf_clone(errbuf));
	}
	else 
	{
		spinlock_release(&session->ses_lock);
	}
	
	/** false because connection is not available anymore */
	*succp = false;
}

/** to be inline'd */
/** 
 * @node Acquires lock to router client session if it is not closed.
 *
 * Parameters:
 * @param rses - in, use
 *          
 *
 * @return true if router session was not closed. If return value is true
 * it means that router is locked, and must be unlocked later. False, if
 * router was closed before lock was acquired.
 *
 * 
 * @details (write detailed description here)
 *
 */
static bool rses_begin_locked_router_action(
        ROUTER_CLIENT_SES* rses)
{
        bool succp = false;
        
        CHK_CLIENT_RSES(rses);

        if (rses->rses_closed) {
                goto return_succp;
        }       
        spinlock_acquire(&rses->rses_lock);
        if (rses->rses_closed) {
                spinlock_release(&rses->rses_lock);
                goto return_succp;
        }
        succp = true;
        
return_succp:
        return succp;
}

/** to be inline'd */
/** 
 * @node Releases router client session lock.
 *
 * Parameters:
 * @param rses - <usage>
 *          <description>
 *
 * @return void
 *
 * 
 * @details (write detailed description here)
 *
 */
static void rses_end_locked_router_action(
        ROUTER_CLIENT_SES* rses)
{
        CHK_CLIENT_RSES(rses);
        spinlock_release(&rses->rses_lock);
}


static uint8_t getCapabilities(
        ROUTER*  inst,
        void*    router_session)
{
        return 0;
}

/********************************
 * This routine returns the root master server from MySQL replication tree
 * Get the root Master rule:
 *
 * find server with the lowest replication depth level
 * and the SERVER_MASTER bitval
 * Servers are checked even if they are in 'maintenance'
 *
 * @param servers	The list of servers
 * @return		The Master found
 *
 */

static BACKEND *get_root_master(BACKEND **servers) {
	int i = 0;
	BACKEND *master_host = NULL;

	for (i = 0; servers[i]; i++) {
		if (servers[i] && (servers[i]->server->status & (SERVER_MASTER|SERVER_MAINT)) == SERVER_MASTER) {
			if (master_host && servers[i]->server->depth < master_host->server->depth) {
				master_host = servers[i];
			} else {
				if (master_host == NULL) {
					master_host = servers[i];
				}
			}
		}
	}
	return master_host;
}

static int handle_state_switch(DCB* dcb,DCB_REASON reason, void * routersession)
{
    ss_dassert(dcb != NULL);

    /* Airproxy handles non-responding server connection */
    if (DCB_IS_IN_CONN_POOL(dcb)) {
        ss_dassert(reason == DCB_REASON_NOT_RESPONDING);
        if (reason == DCB_REASON_NOT_RESPONDING) {
            server_backend_connection_not_responding_cb(dcb);
            return 0;
        } else {
            /* Note: bypass useless variables initialization, and rework if
             * they are indeed used in the future */
            goto handle_reason;
        }
    }

    SESSION* session = dcb->session;
    ROUTER_CLIENT_SES* rses = (ROUTER_CLIENT_SES*)routersession;
    SERVICE* service = session->service;
    ROUTER* router = (ROUTER *)service->router;

handle_reason:
    switch(reason)
    {
	case DCB_REASON_CLOSE:
        dcb->func.close(dcb);
        break;
    case DCB_REASON_DRAINED:
        /** Do we need to do anything? */
        break;
    case DCB_REASON_HIGH_WATER:
        /** Do we need to do anything? */
        break;
    case DCB_REASON_LOW_WATER:
        /** Do we need to do anything? */
        break;
    case DCB_REASON_ERROR:
        dcb->func.error(dcb);
        break;
    case DCB_REASON_HUP:
        dcb->func.hangup(dcb);
        break;
    case DCB_REASON_NOT_RESPONDING:
        dcb->func.hangup(dcb);
        break;
    default:
        break;
    }

    return 0;
}

/**
 * Airbnb connection proxy read connection router pooling callbacks
 */

static void
link_dcb_router_session(DCB *backend_dcb, ROUTER_CLIENT_SES *rses)
{
    /* dcb->user was cleared when unparked from persistent connections pool */
    ss_dassert(backend_dcb->user == NULL);
    /* link the backend_dcb with router session backend ref */
    DCB_SET_ROUTER_SESSION(backend_dcb, rses, -1);
    rses->backend_dcb = backend_dcb;
    rses->rses_in_pool = false;
    /* link the backend_dcb with callback data for dcb_add_callback */
    backend_dcb->callbacks->userdata = rses;
}

static void
unlink_dcb_router_session(DCB *backend_dcb)
{
    ROUTER_CLIENT_SES *rses = (ROUTER_CLIENT_SES *)DCB_GET_ROUTER_SESSION(backend_dcb);
    /* mark router session backend ref as IN_POOL and reset DCB pointer */
    rses->rses_in_pool = true;
    rses->backend_dcb = NULL;
    /* clear router session back pointer, it will be set when a backend_dcb
     * is chosen to link with a client session */
    DCB_SET_ROUTER_SESSION(backend_dcb, NULL, -1);
    /* clear the backend_dcb callback data i.e. router session */
    backend_dcb->callbacks->userdata = NULL;
}

static void
enqueue_server_connection_pool(ROUTER_CLIENT_SES *rses, GWBUF *querybuf)
{
    SERVER *server = NULL;
    POOL_QUEUE_ITEM *queue_item = &rses->rses_queue_item;
    int max_queue_depth = config_server_connection_pool_throttle();

    ss_dassert(rses->backend_dcb == NULL);
    server = rses->backend->server;
    ss_dassert(server != NULL);

    /* If the server connection pool queue depth is over threshold, kill client
     * session so that it would not be penalized with query elevated latency */
    if (!SERVER_IS_RUNNING(server) ||
        server->conn_pool.pool_stats.n_queue_items >= max_queue_depth)
    {
        /* must release router session lock before terminating client session */
        rses_end_locked_router_action(rses);
        gwbuf_free(querybuf);
        dcb_close(rses->rses_client_session->client);
        atomic_add(&server->conn_pool.pool_stats.n_throttled_queue_reqs, 1);
        LOGIF(LE, (skygw_log_write_flush(
                     LOGFILE_ERROR,
                     "%lu [enqueue_server_connection_pool] Close client session "
                     "due to connection pool queue depth over threshold.",
                     pthread_self())));
        return;
    }

    LOGIF(LD, (skygw_log_write(
            LOGFILE_DEBUG,
            "%lu [enqueue_server_connection_pool] router session %p to server %p "
            "for client session %p client_dcb %p",
            pthread_self(), rses, server,
	    rses->rses_client_session, rses->rses_client_session->client)));
    ss_dassert(queue_item->router_session == rses);
    queue_item->next = NULL;
    queue_item->query_buf = querybuf;
    server_enqueue_connection_pool_request(server, queue_item);
}

static void
dequeue_server_connection_pool(ROUTER_CLIENT_SES *rses)
{
    SERVER *server = NULL;
    POOL_QUEUE_ITEM *queue_item = &rses->rses_queue_item;

    ss_dassert(queue_item->router_session == rses);
    if (queue_item->query_buf == NULL)
        return;
    server = rses->backend->server;
    ss_dassert(server != NULL);
    server_remove_connection_pool_request(server, queue_item);
    /* if called for housekeeper cleanup of client sessions, skip buffer free
     * in risk of memory leak because housekeeper would crash attempting to
     * free embedded_thd in parsing_info_done. Scheduled rolling proxy restart
     * will take care of the rare memory leak. */
    if (!DCB_IS_IN_HK_CLEANUP(rses->rses_client_session->client) && queue_item->query_buf) {
        /* queue item has sole ownership of the querybuf */
        gwbuf_free(queue_item->query_buf);
    }
    /* clear the embedded POOL_QUEUE_ITEM */
    queue_item->query_buf = queue_item->next = NULL;
}

/**
 * This is the connection pool engaging entry point on the query route path.
 * It first looks for an available connection in the pool. If no luck, it
 * enqueues the router session in server connection pool. 
 *
 * @return  0 a connection is found and linked with the router session 
 * @return  1 no connection and router session is enqueued.
 * @return -1 faulty situation
 *
 * FIXME(liang) consolidate return value with failure scenario handling
 */
static int
try_server_connection_or_enqueue(DCB **p_dcb, ROUTER_CLIENT_SES *rses,
				 GWBUF *querybuf)
{
    int rc;
    SESSION *client_session = NULL;

    /* must acquire router session lock because caller has released it */
    if (rses == NULL || !rses_begin_locked_router_action(rses))
        return -1;

    ss_dassert(rses->backend_dcb == NULL && rses->rses_client_session != NULL);
    ss_dassert(SERVER_USE_CONN_POOL(rses->backend->server));
    client_session = rses->rses_client_session;
    if (pool_unpark_connection(&rses->backend_dcb, client_session,
			       rses->backend->server,
			       client_session->client->user, rses))
    {
        rc = 0;
        *p_dcb = rses->backend_dcb;
    }
    /* no available backend connection, it will be queued in server connection pool */
    else
    {
        GWBUF *querybuf_clone = gwbuf_clone(querybuf);
        enqueue_server_connection_pool(rses, querybuf_clone);
        rc = 1;
    }

    rses_end_locked_router_action(rses);
    return rc;
}

static bool
forward_request_query(ROUTER_CLIENT_SES *rses, GWBUF *querybuf, DCB *backend_dcb)
{
    int rc;
    SERVER *server;

    LOGIF(LD, (skygw_log_write(
            LOGFILE_DEBUG,
            "%lu [forward_request_query] router session %p backend_dcb %p for server %p",
            pthread_self(), rses, backend_dcb, backend_dcb->server)));

    if (!rses_begin_locked_router_action(rses))
        return false;

    /* link backend dcb with the client router session queued in server pool */
    link_dcb_router_session(backend_dcb, rses);
    /* link backend dcb with the client session for response forwarding */
    ss_dassert(rses->rses_client_session != NULL);
    if (!session_link_dcb(rses->rses_client_session, backend_dcb)) {
        /* client session had gone away, no need of forwarding */
        LOGIF((LE|LT), (skygw_log_write_flush(
              LOGFILE_ERROR,
              "Error : unable to link with client session %p (dcb &p), no query frowarding",
              rses->rses_client_session, rses->rses_client_session->client)));
        gwbuf_free(querybuf);
        rses_end_locked_router_action(rses);
        return false;
    }
    server = rses->backend->server;
    rses_end_locked_router_action(rses);

    /* forward query to backend server */
    rc = backend_dcb->func.write(backend_dcb, querybuf);
    if (rc == 1) {
        START_MYSQL_QUERY_EXEC_TIMER(rses);
        atomic_add(&rses->rses_router->stats.n_queries, 1);
        /* track router client session query state */
        rses->rses_conn_pool_data.query_state = QUERY_ROUTED;
    } else {
        LOGIF((LE|LT), (skygw_log_write_flush(
               LOGFILE_ERROR, "Error : server %s routing query failed.",
               server->name)));
        atomic_add(&server->conn_pool.pool_stats.n_query_routing_errors, 1);
        /* close client connection and router session because of backend failure */
        pool_handle_backend_failure(backend_dcb);
        return false;
    }
    return true;
}

/**
 * This callback is invoked at the end of router session, i.e. after result set
 * forwarding from backend server to client. If there is pending request in
 * queue, it picks up and serve it; else it returns to the connection pool.
 *
 * It assumes that the caller have locked router session for connection DCB
 * runtime data change.
 */
static int
server_backend_connection_pool_cb(DCB *backend_dcb)
{
    bool park_dcb = true;
    SERVER *server = NULL;
    ROUTER_CLIENT_SES *rses = NULL;

    /* FIXME(liang) should close it since it is in wrong state */
    if (backend_dcb->state != DCB_STATE_POLLING)
        return 1;
    ss_dassert(backend_dcb->session != NULL);
    rses = (ROUTER_CLIENT_SES *)DCB_GET_ROUTER_SESSION(backend_dcb);

    /* lock router session for connection DCB runtime linkage state change */
    if (!rses_begin_locked_router_action(rses))
        return 1;

    /* measure query execution elapsed time and maintain server level stats */
    measure_query_elapsed_time_micros(rses->rses_conn_pool_data.query_start,
                                      rses->rses_conn_pool_data.query_exec_start);

    /* track minutely query response data size */
    if (!DCB_IS_IN_AUTH_PHASE(backend_dcb)) {
        track_query_resultset_stats(&backend_dcb->dcb_conn_pool_data.resp_state);
    }

    server = rses->backend->server;
    ss_dassert(SERVER_USE_CONN_POOL(server));
    ss_dassert(rses->rses_conn_pool_data.query_state == QUERY_IDLE);

    /* check out existing connection pool request and serve directly, if any */
    if (DCB_IS_IN_CONN_POOL(backend_dcb) && !SERVER_CONN_POOL_QUEUE_EMPTY(server)) {
        POOL_QUEUE_ITEM *req = NULL;

        /* unlink this backend connection from router session and client session */
        session_unlink_dcb(backend_dcb->session, backend_dcb);
        unlink_dcb_router_session(backend_dcb);
        /* reset query response state before query routing */
        protocol_reset_query_response_state(backend_dcb);
        req = server_dequeue_connection_pool_request(server);
        if (req != NULL) {
            forward_request_query((ROUTER_CLIENT_SES *)req->router_session,
                                (GWBUF *)req->query_buf, backend_dcb);
            /* clear the embedded POOL_QUEUE_ITEM, and query_buf should have
	     * been consumed and therefore no need of gwbuf_free here */
            req->query_buf = req->next = NULL;
            park_dcb = false;
        }
    }

    rses_end_locked_router_action(rses);

    if (park_dcb) {
        pool_park_connection(backend_dcb);
    }

    return 0;
}

static int
server_backend_connection_pool_link_cb(DCB *backend_dcb, int link_mode,
				       int rses_locked, void *arg)
{
    ROUTER_CLIENT_SES *rses = NULL;
    ss_dassert(link_mode == 0 || arg != NULL);
    rses = (ROUTER_CLIENT_SES*)(link_mode == 1 ?
				arg : DCB_GET_ROUTER_SESSION(backend_dcb));
    if (!rses_locked) {
        ss_dassert(rses != NULL);
        if (!rses_begin_locked_router_action(rses))
	    return 1;
    }

    if (link_mode == 1) { /* link */
        ss_dassert(arg != NULL);
        link_dcb_router_session(backend_dcb, (ROUTER_CLIENT_SES *)arg);
    } else { /* unlink */
        unlink_dcb_router_session(backend_dcb);
    }

    if (!rses_locked)
        rses_end_locked_router_action(rses);
    return 0;
}

static void
init_connection_pool_dcb(DCB *backend_dcb, ROUTER_CLIENT_SES *rses,
                         ROUTER_INSTANCE *router)
{
    /* register router specific connection pooling callback */
    backend_dcb->conn_pool_func = &conn_pool_cb;
    /* register router session back pointer */
    DCB_SET_ROUTER_SESSION(backend_dcb, rses, -1);
    /* back pointer to client session and router instance */
    rses->rses_client_session = backend_dcb->session;
    rses->rses_router = router;
}

static void
router_conn_pool_stats_cb(void *router_instance, void *stats_arg)
{
    ROUTER_INSTANCE *router = (ROUTER_INSTANCE *)router_instance;
    service_conn_pool_minutely_stats *stats = (service_conn_pool_minutely_stats *)stats_arg;
    stats->n_queries_routed = router->stats.n_queries;
}
