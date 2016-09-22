# Airbnb MaxScale Configurations

Airbnb MaxScale introduced several new configurable parameters. Please see [design doc](airbnb_maxscale_design_doc.md) for some more details.

## Server Parameters

### connection_pool_size

This parameter configures the number of connections in a server connection pool. A value of zero means no connection pooling mode. Because connection pool is created for each backend MySQL server, this parameter must be put in the server section of MaxScale configuration file.

### server_connection_pool_user

In Airbnb MaxScale, connections in a server connection pool are successfully authenticated for a particular user, and only same users client sessions are allowed to use a connection in the pool. Please see [design doc](airbnb_maxscale_design_doc.md) for more details.

### service_health_check_user

For service discovery, a health check agent can be configured to talk to a MaxScale server instance. This user is configured to perform connection request with MaxScale server for health checking purpose.

### log_session_command_error

In Airbnb MaxScale, session commands are treated as no-op in connection pooling mode. A success response is returned to client, and an error message is logged. This parameter turns on/off the logging session commands rejection messages.

## Example Config File

We use SmartStack for service discovery at Airbnb. In the example configuration file, the backend MySQL database is a service `airbed_db.synapse`, which could be a single MySQL master or a set of MySQL read replicas.

```
# This is a template for Airbnb MaxScale dbproxy configuration file.

[maxscale]
threads=1
server_connection_pool_user=app_user
service_health_check_user=health_user
log_session_command_error=false

[MySQL Monitor]
type=monitor
module=mysqlmon
servers=airbed_db
user=maxscale_monitor
passwd=xxxxxxxxxxx
monitor_interval=10000

[qla]
type=filter
module=qlafilter
options=/tmp/QueryLog

[fetch]
type=filter
module=regexfilter
match=fetch
replace=select

[hint]
type=filter
module=hintfilter

## Definitions of services

[read_write_router]
type=service
router=readwritesplit
servers=airbed_db
user=maxscale
passwd=yyyyyyyyyyyy
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

[airmaster]
type=server
address=airbed_db.synapse
port=3230
protocol=MySQLBackend
connection_pool_size=10
```
