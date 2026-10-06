#!/bin/sh
### BEGIN INIT INFO
# Provides:          rkkvm-video
# Required-Start:    $local_fs
# Required-Stop:     $local_fs
# Default-Start:     S
# Default-Stop:      K
# Description:       RK3506 IP-KVM userspace UVC MJPEG streamer
### END INIT INFO

DAEMON=/userdata/rkkvm-video
PIDFILE=/var/run/rkkvm-video.pid
LOG=/var/log/rkkvm-video.log

start()
{
	echo "Starting rkkvm-video"
	start-stop-daemon -S -b -m -p "$PIDFILE" -x /bin/sh -- \
		-c "exec $DAEMON >>$LOG 2>&1"
}

stop()
{
	echo "Stopping rkkvm-video"
	pid=$(cat "$PIDFILE" 2>/dev/null)
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	i=0
	while [ $i -lt 12 ]; do
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.5
		i=$((i + 1))
	done
	# kill any stragglers by name, then give UVC time to be released
	killall rkkvm-video 2>/dev/null
	i=0
	while [ $i -lt 12 ]; do
		pidof rkkvm-video >/dev/null 2>&1 || break
		sleep 0.5
		i=$((i + 1))
	done
	# force kill if still alive
	for p in $(pidof rkkvm-video); do kill -9 "$p" 2>/dev/null; done
	sleep 2
	rm -f "$PIDFILE"
}

case "$1" in
	start)   start ;;
	stop)    stop ;;
	restart) stop; start ;;
	*)       echo "Usage: $0 {start|stop|restart}"; exit 3 ;;
esac
