#!/bin/sh
### BEGIN INIT INFO
# Provides:          rkkvm-audio
# Required-Start:    $local_fs
# Default-Start:     S
# Default-Stop:      K
# Description:       Capture audio -> FIFOs for RK-KVM.
#                    /run/rk.pcm  : UAC (HDMI audio, no wiring)
#                    /run/rk2.pcm : board ALSA input (needs a cable)
### END INIT INFO

CONF=/userdata/audio.conf
AUDIO_DEV=hw:0,0
AUDIO_RATE=48000
AUDIO_CH=2
AUDIO_ENABLE=1
[ -f "$CONF" ] && . "$CONF"

FIFO_UAC=/run/rk.pcm
FIFO_ALSA=/run/rk2.pcm
PIDFILE=/var/run/rkkvm-audio.pid
LOG=/var/log/rkkvm-audio.log

start()
{
	[ "$AUDIO_ENABLE" = "0" ] && { echo "audio disabled"; return; }
	echo "Starting rkkvm-audio (UAC + ALSA)"
	[ -p "$FIFO_UAC" ] || mkfifo "$FIFO_UAC" 2>/dev/null
	[ -p "$FIFO_ALSA" ] || mkfifo "$FIFO_ALSA" 2>/dev/null
	chmod 666 "$FIFO_UAC" "$FIFO_ALSA" 2>/dev/null
	UAC="while true; do /userdata/uac_capture $FIFO_UAC; sleep 1; done"
	ALSA="while true; do arecord -q -D $AUDIO_DEV -f S16_LE -r $AUDIO_RATE -c $AUDIO_CH -t raw - 2>>$LOG > $FIFO_ALSA; sleep 1; done"
	start-stop-daemon -S -b -m -p "$PIDFILE" -x /bin/sh -- -c \
	  "$UAC & $ALSA & wait"
}

stop()
{
	echo "Stopping rkkvm-audio"
	start-stop-daemon -K -p "$PIDFILE" 2>/dev/null
	killall uac_capture arecord 2>/dev/null
	rm -f "$PIDFILE"
}

case "$1" in
	start)   start ;;
	stop)    stop ;;
	restart) stop; sleep 1; start ;;
	*)       echo "Usage: $0 {start|stop|restart}"; exit 3 ;;
esac
