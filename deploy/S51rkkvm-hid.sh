#!/bin/sh
### BEGIN INIT INFO
# Provides:          rkkvm-hid
# Required-Start:    $local_fs
# Required-Stop:     $local_fs
# Default-Start:     S
# Default-Stop:      K
# Description:       RK3506 IP-KVM userspace USB HID gadget
### END INIT INFO

DAEMON=/userdata/rkkvm-hid
PIDFILE=/var/run/rkkvm-hid.pid
LOG=/var/log/rkkvm-hid.log

start()
{
	echo "Starting rkkvm-hid"
	# Take the OTG gadget over from the stock usbdevice/adb service.
	# Kill the ffs userspace process BEFORE unbinding the UDC.
	killall usbdevice 2>/dev/null
	killall adbd 2>/dev/null
	sleep 1
	if [ -e /sys/kernel/config/usb_gadget/rockchip/UDC ]; then
		echo "" > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null
	fi
	start-stop-daemon -S -b -m -p "$PIDFILE" -x /bin/sh -- \
		-c "exec $DAEMON >>$LOG 2>&1"
}

stop()
{
	# Only kill the daemon. Never touch configfs here: unbinding the ffs
	# gadget after use wedges configfs on this kernel. configfs is reset
	# on reboot anyway.
	echo "Stopping rkkvm-hid"
	pid=$(cat "$PIDFILE" 2>/dev/null)
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	rm -f "$PIDFILE"
}

case "$1" in
	start)   start ;;
	stop)    stop ;;
	restart) stop; sleep 1; start ;;
	*)       echo "Usage: $0 {start|stop|restart}"; exit 3 ;;
esac
