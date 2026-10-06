#!/bin/sh
### BEGIN INIT INFO
# Provides:          rkkvm-audio
# Required-Start:    $local_fs
# Default-Start:     S
# Default-Stop:      K
# Description:       Capture audio -> FIFO for RK-KVM.
#                    Prefers userspace UAC (HDMI audio from the capture stick);
#                    falls back to ALSA arecord.
### END INIT INFO

CONF=/userdata/audio.conf
AUDIO_SRC=uac
AUDIO_DEV=hw:0,0
AUDIO_RATE=48000
AUDIO_CH=2
AUDIO_ENABLE=1
[ -f "$CONF" ] && . "$CONF"

FIFO=/run/rk.pcm
PIDFILE=/var/run/rkkvm-audio.pid
LOG=/var/log/rkkvm-audio.log

start()
{
	[ "$AUDIO_ENABLE" = "0" ] && { echo "audio disabled"; return; }
	[ -p "$FIFO" ] || mkfifo "$FIFO" 2>/dev/null
	chmod 666 "$FIFO" 2>/dev/null

	if [ "$AUDIO_SRC" = "uac" ] && [ -x /userdata/uac_capture ]; then
		echo "Starting rkkvm-audio (UAC HDMI audio)"
		CMD='/userdata/uac_capture /run/rk.pcm'
	else
		echo "Starting rkkvm-audio (ALSA $AUDIO_DEV)"
		CMD="arecord -q -D $AUDIO_DEV -f S16_LE -r $AUDIO_RATE -c $AUDIO_CH -t raw - 2>>$LOG > $FIFO"
	fi
	start-stop-daemon -S -b -m -p "$PIDFILE" -x /bin/sh -- -c \
	  "while true; do $CMD; sleep 1; done"
}

stop()
{
	echo "Stopping rkkvm-audio"
	start-stop-daemon -K -p "$PIDFILE" 2>/dev/null
	killall arecord uac_capture 2>/dev/null
	rm -f "$PIDFILE"
}

case "$1" in
	start)   start ;;
	stop)    stop ;;
	restart) stop; sleep 1; start ;;
	*)       echo "Usage: $0 {start|stop|restart}"; exit 3 ;;
esac
