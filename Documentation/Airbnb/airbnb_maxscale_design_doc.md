
# Airbnb MaxScale Design Doc

Airbnb MaxScale implements connection pooling for efficient backend connections sharing.

## Connection Pooling

Maxscale manages a 1-to-1 connection linkage between a client connection and backend connection. Critical data structures from `readwritesplit`router service connection management point of view include the following,

- client_dcb, client connection control block.
- client_session, client session status block associated with a client_dcb.
- router_client_session, router service session created for a client_dcb.
- backend_bref, backend reference to a particular MySQL server, and is managed by router_client_session.
- backend_dcb, backend server connection control block, and is referenced by backend_bref.

If a router service is responsible for routing queries to multiple MySQL backend servers, a router_client_session has an array of backend_brefs, each referring to a backend_dcb that is connected to a backend server. The query and response routing flow is straightforward with the 1-on-1 connection linkage mode.

### Connection Pooling

Connection pooling in Airbnb Maxscale is implemented by multiplexing N client connections over M backend connections to a MySQL server. After a client connection request complete successful authentication with a backend MySQL server, it unlinks the backend_dcb, by severing the reference link between a backend_bref and the backend_dcb. The backend_dcb no longer references the client_dcb and client_session, and enters the server connection pool.

For a particular backend MySQL server, all backend connections (i.e. backend_dcb) are managed by the server's persistent connections list. Airbnb MaxScale connection pool leverages the persistent connections feature in Maxscale 1.3 development branch.

A Maxscale server config parameter `connection_pool_size` is added to control the connection pooling feature. A zero value of this parameter means no connection pooling. Else, its value specifies the max number of backend connections that will be kept in the pool.

```
[dbreplay-airmaster]
type=server
address=dbreplay-airmaster.synapse
port=3806
protocol=MySQLBackend
connection_pool_size=10
```

For query routing a new client query request, the Maxscale connection proxy picks up a pooling backend connection, links it with the client session, and forwards query buffer on the `backend_dcb`, and sets the `backend_dcb` to wait for response from the backend server for response forwarding. If all pooling backend connections are in use, a new client query request will enqueue in a server's connection pool queue and wait to be served by available backend connection. By the time a client session is ready to be paired with an available backend connection, the routing decision had been made and therefore it knows which server to send query to.

### Transaction Aware Pooling

It's critical to maintain the linkage between a `client_dcb` and `backend_dcb` if the client session is within a transaction context. Maxscale presumes autocommit is on by default, just like MySQL does. If client starts a transaction with BEGIN TRANSACTION and closes it with COMMIT, Maxscale marks `router_client_session` active in transaction. The connection pool management would not unlink `backend_dcb` if the current `router_client_session` has active transaction. In the default autocommit mode, Maxscale unlinks `backend_dcb` from `client_session` after forwarding response data for each statement.

### Query Resultset Forwarding

In the 1-to-1 connection mode, the associated backend connection control block for a client connection is permanent until client disconnects. The ever present link between client and backend server is easy for routing of both query request and query result. A MySQL result set consists of one or more MySQL packets for normal response to MySQL query. All packets may not be received by the proxy continually. It is not a problem for the 1-to-1 linkage mode since the Maxscale proxy forwards all packets to the linked client as they come. However, it could be a challenge to connection pooling proxy if the proxy didn't know how and when it has forwarded the complete result set to the client. Prematurely unlinking a backend connection control block from a client connection before delivering entire result set would cause the client to hang indefinitely in hope of waiting for complete resultset.

Airbnb Maxscale implements correct MySQL query resultset forwarding by following MySQL client server protocol ProtocolText::Resultset for COM_QUERY_RESPONSE. A complete set of mysql packets for a normal query result is as follows,

- column count packet
- N column definition packets, one packet for each column
- EOF packet
- R row data packets, one packet for each row
- EOF packet (or ERR packet), and R is encoded in EOF packet

For query that results in failure, it is one single ERR packet. For session commands, it is one single OK packet.

The connection pooling dbproxy receives and parses the entire result set for the query issued by the current client. It unlinks the backend connection with a client after it concludes that it has received all mysql packets.

Note that multi-resultset is currently not supported. Multi-resultset is used by MySQL stored procedure, which is not used in production at Airbnb. It should not be hard to extend to support multi-resultset.

## Practical Design Choices

### A single worker thread model

MariaDB's MaxScale does provide a multi-worker thread model. All worker threads share the same internal data structures (e.g. server and router instance etc), and MaxScale uses lock to protect internal data structures. The number of worker threads is controlled by MaxScale proxy parameter `threads`.

A multi-worker thread model may provide limited performance improvement, but it adds risk of lock contention and various race conditions that are time-consuming to debug. A single worker thread, in theory, works well for a routing proxy server because it does not add too much processing overhead and can be fast at churning requests as they come. While we still use lock to protect the server connection pool and client request queue, a single worker thread model has no risk of lock contention and race condition. Given the time that we had during development of Airbnb MaxScale, we chose single worker thread mode for its simplicity.

```
[maxscale]
threads=1
```

__Note that using multiple worker threads in Aribnb MaxScale will likely cause bugs and locking issue.__

In production at Airbnb, we chose to achieve concurrency by running multiple Airbnb MaxScale server instances.

### No support of session commands

In MaxScale, a session command must be routed to all backend servers before it is considered complete. Maxscale maintains a complicated session command routing logic. In Airbnb MaxScale connection pooling mode, routing a session command to all MySQL servers makes no sense. This is because a MySQL client session at the backend MySQL server side is no longer specific to any client. In Airbnb application, we do not use any session command, e.g. disable autocommit or set global/session variable. A client connection is created with database selected. Therefore, we explicitly reject all session commands, in Airbnb MaxScale.

### Support a single router service in a production Maxscale dbproxy service

Each router service listens to new client connections on its dedicated service port. Maxscale can run multiple different types of router services, as long as clients know which port to send connection requests. This does not make much sense in our production environment at Airbnb. This constrain is expressed in the database proxy service definition, by specifying router service type for a particular dbproxy service. All Airbnb Maxscale server instances of that dbproxy service will run the designated router service.

### Allow a single user to use a production Maxscale dbproxy service

We leverage Maxscale server persistent connections for connection pool management. One criterion for pairing a pooling backend connection with a client session is that the backend connection was authenticated for the same user as the client session. Therefore, it'd be bad to mix `backend_dcb` that belong to different users in the same server connection pool. In Airbnb applications, an application user name is used to connect production databases. A single designated user makes sense for our production environment, and therefore we chose to restrict an Aribnb MaxScale server to a dedicated user.

It is possible to support multiple connection pools for different users. However, even in that mode, it would be good to have a limited set of users that are allowed to connect to the same database proxy.

## Appendix

Example Aribnb MaxScale configuration file.

```
[maxscale]
threads=1

[MySQL Monitor]
type=monitor
module=mysqlmon
servers=dbreplay-airmaster
user=maxscale_monitor
passwd=xxxxxxxxxx
monitor_interval=10000

## Definitions of services
[read_write_router]
type=service
router=readwritesplit
servers=dbreplay-airmaster
user=maxscale
passwd=yyyyyyyyyy
max_slave_connections=100%
enable_root_user=1
[Debug Interface]
type=service
router=debugcli
[CLI]
type=service
router=cli

## Definitions of service listeners
[read_write_router_listener]
type=listener
service=read_write_router
protocol=MySQLClient
port=4006
[Debug Listener]
type=listener
service=Debug Interface
protocol=telnetd
address=127.0.0.1
port=4442
[CLI Listener]
type=listener
service=CLI
protocol=maxscaled
port=6603

## Definition of backend mysql servers
[dbreplay-airmaster]
type=server
address=dbreplay-airmaster.synapse
port=3806
protocol=MySQLBackend
connection_pool_size=10
```
