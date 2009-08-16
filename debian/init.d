#!/bin/sh
### BEGIN INIT INFO
# Provides:          lighttpd1.5
# Required-Start:    networking
# Required-Stop:     networking
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Start the lighttpd web server.
### END INIT INFO


PATH=/sbin:/bin:/usr/sbin:/usr/bin
DAEMON=/usr/sbin/lighttpd1.5
NAME=lighttpd1.5
DESC="web server"
PIDFILE=/var/run/$NAME.pid
SCRIPTNAME=/etc/init.d/$NAME

DAEMON_OPTS="-f /etc/lighttpd1.5/lighttpd.conf"

test -x $DAEMON || exit 0

set -e

# be sure there is a /var/run/lighttpd1.5, even with tmpfs
mkdir -p /var/run/lighttpd1.5 > /dev/null 2> /dev/null
chown www-data:www-data /var/run/lighttpd1.5
chmod 0750 /var/run/lighttpd1.5

. /lib/lsb/init-functions

case "$1" in
  start)
	log_daemon_msg "Starting $DESC" $NAME
	if ! start-stop-daemon --start --quiet\
	--pidfile $PIDFILE --exec $DAEMON -- $DAEMON_OPTS ; then
            log_end_msg 1
	else
            log_end_msg 0
	fi
    ;;
  stop)
	log_daemon_msg "Stopping $DESC" $NAME
	if start-stop-daemon --quiet --stop --oknodo --retry 30\
	--pidfile $PIDFILE --exec $DAEMON; then
	    rm -f $PIDFILE
	    log_end_msg 0
	else
	    log_end_msg 1
	fi
	;;
  reload)
	log_daemon_msg "Reloading $DESC configuration" $NAME
	if start-stop-daemon --stop --signal 2 --oknodo --retry 30\
	--quiet --pidfile $PIDFILE --exec $DAEMON; then
	    if start-stop-daemon --start --quiet  \
		--pidfile $PIDFILE --exec $DAEMON -- $DAEMON_OPTS ; then
		log_end_msg 0
	    else
		log_end_msg 1
	    fi
	else
	    log_end_msg 1
	fi
  ;;
  restart|force-reload)
	$0 stop
	[ -r  $PIDFILE ] && while pidof lighttpd1.5 |\
		 grep -q `cat $PIDFILE 2>/dev/null` 2>/dev/null ; do sleep 1; done
	$0 start
	;;
  *)
	echo "Usage: $SCRIPTNAME {start|stop|restart|reload|force-reload}" >&2
	exit 1
	;;
esac

exit 0
