#!/bin/sh
# gpiotest <A> <B> : test whether GPIO B follows GPIO A (i.e. A and B are shorted)
# Only drives the two pins you give it -- safe for a header jumper pair.
A="$1"; B="$2"
case "$A$B" in *[!0-9]*|"") echo "usage: gpiotest <gpioA> <gpioB>"; exit 2;; esac

exp() { [ -d /sys/class/gpio/gpio$1 ] || echo "$1" > /sys/class/gpio/export 2>/dev/null; }
exp "$A"; exp "$B"
[ -d /sys/class/gpio/gpio$A ] || { echo "gpio$A unavailable (in use?)"; exit 1; }
[ -d /sys/class/gpio/gpio$B ] || { echo "gpio$B unavailable (in use?)"; exit 1; }

echo in > /sys/class/gpio/gpio$A/direction
echo in > /sys/class/gpio/gpio$B/direction

# baseline (both floating / pulled)
Vb=$(cat /sys/class/gpio/gpio$B/value)

# drive A low
echo out > /sys/class/gpio/gpio$A/direction
echo 0 > /sys/class/gpio/gpio$A/value
sleep 1
V0=$(cat /sys/class/gpio/gpio$B/value)

# drive A high
echo 1 > /sys/class/gpio/gpio$A/value
sleep 1
V1=$(cat /sys/class/gpio/gpio$B/value)

# release
echo in > /sys/class/gpio/gpio$A/direction

echo "gpio$A <-> gpio$B : baseline(B)=$Vb  A=0 -> B=$V0   A=1 -> B=$V1"
if [ "$V0" = 0 ] && [ "$V1" = 1 ]; then
	echo "RESULT: B follows A  =>  A and B are (likely) SHORTED"
elif [ "$V0" = 1 ] && [ "$V1" = 0 ]; then
	echo "RESULT: B is inverse of A (unusual)"
else
	echo "RESULT: B does NOT follow A  =>  not connected"
fi
