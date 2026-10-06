#!/bin/sh
### BEGIN INIT INFO
# Provides:          kvmgpio
# Required-Start:    $local_fs
# Default-Start:     S
# Default-Stop:      K
# Description:       Configure KVM power/reset/status GPIOs at boot
### END INIT INFO

CONF=/userdata/kvm.conf
POWER_GPIO=22
RESET_GPIO=23
STATUS_GPIO=-1

[ -f "$CONF" ] && . "$CONF"

set_out_low() {
	n="$1"
	[ -z "$n" ] && return
	[ "$n" -lt 0 ] 2>/dev/null && return
	[ -d /sys/class/gpio/gpio$n ] || echo "$n" > /sys/class/gpio/export 2>/dev/null
	[ -d /sys/class/gpio/gpio$n ] || return
	echo low > /sys/class/gpio/gpio$n/direction 2>/dev/null || echo out > /sys/class/gpio/gpio$n/direction
	echo 0 > /sys/class/gpio/gpio$n/value 2>/dev/null
}

set_in() {
	n="$1"
	[ -z "$n" ] && return
	[ "$n" -lt 0 ] 2>/dev/null && return
	[ -d /sys/class/gpio/gpio$n ] || echo "$n" > /sys/class/gpio/export 2>/dev/null
	[ -d /sys/class/gpio/gpio$n ] || return
	echo in > /sys/class/gpio/gpio$n/direction 2>/dev/null
}

case "$1" in
	start)
		# power/reset idle LOW so the target's button is not held at boot
		set_out_low "$POWER_GPIO"
		set_out_low "$RESET_GPIO"
		set_in "$STATUS_GPIO"
		;;
	stop) ;;
	*)
		echo "Usage: $0 {start|stop}"; exit 3 ;;
esac
