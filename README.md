# Airbnb MaxScale

Airbnb MaxScale is a fork of MariaDB's MaxScale 1.3. The motivation for this project is to use a database proxy with connection pooling to significantly reduce the number of direct connections to our MySQL databases. Airbnb MaxScale had been deployed in production since early 2016, and it is currently powering all core MySQL databases used by the Airbnb web application.

See the original MariaDB MaxScale [README](README_MARIADB.md) for more information about MaxScale.

## Airbnb Features

We have implemented connection pooling and other interesting features.

### Connection Pooling

MaxScale is an intelligent router between applications and MySQL servers. In MariaDB MaxScale, the connection management model is a one-to-one connection between a client connection and backend connection. In Airbnb Maxscale, connection pooling is implemented by multiplexing N client connections over a fixed number of M connections to a backend MySQL server, where N can be significantly larger than M. After a client connection request completes successful authentication with a backend MySQL server, Airbnb MaxScale severs the link between the backend connection and client connection and parks it in a connection pool of the backend server. The server connection pool size is configurable, and is typically a small number. When receiving a query on a client connection, MaxScale picks a backend connection in the pool, links it with the client connection, and forwards the query to the backend MySQL server. MaxScale understands the transaction context of a client session and therefore it knows to keep the linked backend connection until transaction commits. The link must be kept and used for forwarding the query result back to the client.

### Requests Throttling

The server connection pool size is usually configured to 10 in our production environment. Running many instances of Airbnb MaxScale proxy servers, there are typically several hundred database connections to a MySQL server. Previously, only a small portion of connections would be in active use. When an underlying storage outage happens or a bad, expensive query hits the database, query execution becomes slow and the server connection pool runs out on each MaxScale proxy server instance. We take this symptom as a signal that the backend MySQL server may have a spike of concurrent threads running, and MaxScale proactively throttles client requests by killing client connections. In production, the request throttling had proven to be very useful in preventing database incidents due to transient storage system outages.

### Denylist Query Rejection

MaxScale uses an embedded MySQL parser for query routing. It builds a parse tree for every MySQL query in its query classifier module. Airbnb MaxScale leverages the query parse tree for bad query blacklisting. The motivation to build this feature came from a Ruby VM heap memory corruption incident in our production environment. Because of the memory corruption, Rails' ActiveRecord generated a MySQL query statement which was corrupted in such way that its conditional predicate was effectively removed. For instance, one corrupted query that we were lucky to catch before it caused permanent damage in production was a delete statement with `where 0 = 0`. This had put our production databases in danger of serious corruption. This [blog post](http://webuild.envato.com/blog/tracking-down-ruby-heap-corruption/) has a detailed explanation of the nasty Ruby heap corruption problem. Airbnb MaxScale looks for existence of malformed equality conditions in update and delete statements by inspecting the predicate in the query parse tree and rejecting them.

### Monitoring

Airbnb MaxScale implements minutely internal metrics collection and exposes them for real time monitoring. A new `stats` command is added in MaxAdmin tool. External stats agent can be created to pull stats from a running MaxScale server periodically.

## Build

Airbnb MaxScale was forked off the MaxScale 1.3 development branch. All Airbnb features are in the branch `connection_proxy`.

Before building Airbnb MaxScale, make sure you have all prerequisites installed on the build machine. For the sake of example, the Airbnb Maxscale source is in `/srv/dbproxy/MaxScale`, and the dependency libraries are installed in `/srv/dbproxy`.

The following builds a Debian release package. The release package name has an AIRBNB version number in it.

```
mkdir release_maxscale
cd release_maxscale
cmake /srv/dbproxy/MaxScale -DCMAKE_BUILD_TYPE=RelWithDebugInfo -DMYSQL_DIR=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/include/mysql -DEMBEDDED_LIB=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/lib/libmysqld.so -DMYSQLCLIENT_LIBRARIES=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/lib/libmysqlclient.so -DERRMSG=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/share/english/errmsg.sys -DBUILD_TESTS=Y -DWITH_SCRIPTS=Y -DPACKAGE=Y -DCMAKE_INSTALL_PREFIX=/srv/dbproxy
make
make package
```

For a debug build it is slightly different:

```
mkdir debug_maxscale
cd debug_maxscale
cmake /srv/dbproxy/src -DCMAKE_BUILD_TYPE=Debug -DMYSQL_DIR=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/include/mysql -DEMBEDDED_LIB=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/lib/libmysqld.so -DMYSQLCLIENT_LIBRARIES=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/lib/libmysqlclient.so -DERRMSG=/srv/dbproxy/mariadb-10.0.20-linux-x86_64/share/english/errmsg.sys -DBUILD_TESTS=Y -DWITH_SCRIPTS=N -DCMAKE_INSTALL_PREFIX=$HOME/debug_maxscale
make
make install
```

For more information about Airbnb MaxScale, please read [documentations](Documentation/Airbnb).

We welcome feature ideas and pull requests for bugs and features. We are also happy to work with other parties to merge this branch back into the upstream branch for the benefit of the broader community.
