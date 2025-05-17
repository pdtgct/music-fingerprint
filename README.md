# Music Fingerprint

This audio fingerprinter combined the original version of [AcoustID Chromaprint](https://acoustid.org/chromaprint) and [Libfooid](https://github.com/timmartin/libfooid) into a single, more accurate fingerprinting system for identifying music files.

It includes a Postgres GiST index for comparing music fingerprints. (This how we built vector databases back in 2010.)

## Installation

* install the dependencies (there are lots!)
  ```sh
  cd requirements
  ./ubuntu-install
  ```

* run the makefile:

  ```sh
  make
  sudo make install
  ```

`libs` install to `/usr/local/lib/`

* make the python binding

  ```sh
  make python
  ```

* copy the resulting musicfp.so into your python site-packages
  or, for a standard Ubuntu install into dist-packages, typically:

  * on OS X 10.6 (Snow Leopard):
    
    `/Library/Python/2.6/site-packages/`

  * on OS X 10.5 (Leopard, python package install):

    `/Library/Frameworks/Python.framework/Versions/2.6/lib/python2.6/site-packages/`

  * on Ubuntu (tested on 10.04, Lucid)

    `/usr/lib/python2.6/dist-packages/`

* For the postgres GiST index, you need to install postgresql 
  (server and client), then make the bindings

  * install postgresql (current version is 8.4.5; 9.0.1 also tested)

    Ubuntu:
    -------

    ```sh
    sudo apt-get -y install postgresql postgresql-common postgresql-client-common libpq-dev postgresql-server-dev-8.4
    sudo apt-get -y install python-psycopg2
    ```

    OS X (MacPorts):
    ---------------

    * install MacPorts postgresql84-server or postgresql90-server

    ```sh
    sudo port install postgresql84-server
    sudo mkdir -p /opt/local/var/db/postgresql84/defaultdb
    sudo chown postgres:postgres /opt/local/var/db/postgresql84/defaultdb
    sudo /opt/local/lib/postgresql84/bin/initdb -D /opt/local/var/db/postgresql84/defaultdb -U postgres -W
    # add your password at the prompt

    sudo launchctl load /Library/LaunchDaemons/org.macports.postgresql84-server.plist
    # at this point postgres will start up and you can do with it as you will
    # but you want the Python bindings as well.  For both you will need 
    # ARCHFLAGS to build psycopg2.
    ```

    - on OS X 10.6

    ```sh
    ARCHFLAGS="-arch x86_64" CFLAGS="-I/opt/local/lib/postgresql84" LDFLAGS="-L/opt/local/lib/postgresql84" easy_install psycopg2
    ```

    - on OS X 10.5

    ```sh
    ARCHFLAGS="-arch i386" CFLAGS="-I/opt/local/lib/postgresql84" LDFLAGS="-L/opt/local/lib/postgresql84" easy_install psycopg2
    ```

    WARNING: This has been tested with the apt i386 version and MacPorts versions
             but you may want finer-grained control and for development purposes
             it is highly advisable to install the source version with --enable-debug
             and --enable-cassert. Follow the directions for installing from
             source, below.

  * edit `/etc/postgresql/8.4/main/pg_hba.conf` :

    - comment out the line:
    #local  postgres    postgres                         ident
    
    + add the line (this is not very secure):
    local       all         all                          trust

    + add the line (for local connections)
    host        all         all      192.168.0.0/16      md5

  * edit /etc/postgresql/8.4/main/postgresql.conf :

    + uncomment and edit the line (to allow connections from other machines):
    listen_address = '*'

  * build and install the fprint type and GiST index:

    * cd into $TOPDIR/postgres and execute:
    
      ```sh
      make
      sudo make install
      sudo service postgresql-8.4 restart
      # (depending on the database you have, here we are using "postgres")
      psql -U postgres -f pgfprint.sql postgres
      ```

## building Postgresql from Source on Ubuntu 10.04

```sh
apt-get -y install libreadline-dev libpam-dev bison flex libxml2-dev libldap2-dev 

wget http://wwwmaster.postgresql.org/redir/198/h/source/v8.4.5/postgresql-8.4.5.tar.bz2
tar -xf postgresql-8.4.5.tar.bz2

cat <<EOF > config.sh
#!/usr/bin/env bash

./configure --with-openssl \
--with-pam \
--with-krb5 \
--with-gssapi \
--with-ldap \
--enable-thread-safety \
--enable-nls \
--with-libxml \
--with-python
EOF

chmod u+x config.sh
./config.sh
make
sudo make install

sudo adduser postgres

sudo mkdir /usr/local/pgsql/data
sudo chown postgres:postgres /usr/local/pgsql/data

# Below will prompt for a password; enter anything you like since the system 
# startup and shutdown sequences depend on the settings you wrote in `pg_hba.conf`,
# above: "trust".

sudo -u postgres /usr/local/pgsql/bin/initdb -D /usr/local/pgsql/data -U postgres -W

# we need a symlink to pg_config so Make can find it when building the pgfprint.so library
sudo ln -sf /usr/local/pgsql/bin/pg_config /usr/local/bin/

# Add file: /etc/init.d/postgresql-8.4

sudo cat <<EOF > /etc/init.d/postgresql-8.4
#!/bin/sh -e

### BEGIN INIT INFO
# Provides:             postgresql postgresql-8.4
# Required-Start:       $local_fs $remote_fs $network $time
# Required-Stop:        $local_fs $remote_fs $network $time
# Should-Start:         $syslog
# Should-Stop:          $syslog
# Default-Start:        2 3 4 5
# Default-Stop:         0 1 6
# Short-Description:    PostgreSQL 8.4 RDBMS server
### END INIT INFO

# Setting environment variables for the postmaster here does not work; please
# set them in /etc/postgresql/8.4/<cluster>/environment instead.

export PATH=/sbin:/usr/sbin:/usr/bin:/usr/local/pgsql/bin

USER=postgres
PROG="sudo -u $USER /usr/local/pgsql/bin/pg_ctl"
OPTIONS="-D /usr/local/pgsql/data -U $USER -w -l /var/log/postgresql/postgresql-8.4.5.log -m smart -p /usr/local/pgsql/bin/postgres "

case "$1" in
    start)
        $PROG $OPTIONS start
        ;;
    stop)
        $PROG $OPTIONS stop
        ;;
    restart)
        $PROG $OPTIONS restart
        ;;
    force-reload | reload)
        $PROG $OPTIONS reload
        ;;
    status)
        $PROG $OPTIONS status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|reload|force-reload|status|autovac-start|autovac-stop|autovac-restart}"
        exit 1
        ;;
esac

exit 0

EOF

sudo service postgresql-8.4 start

# You should be done setting up postgres. 
# SUGGESTED: Add /usr/local/pgsql/bin to your $PATH so you can call psql directly
#            (/usr/local/pgsql/bin/ items are not symlinked in /usr/local/bin).
```
