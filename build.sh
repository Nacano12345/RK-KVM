#!/usr/bin/env bash
#
# Cross-build RK-KVM for the Tronlong TL3506-MiniEVM (Rockchip RK3506, armv7l).
#
# Produces (all static, musl):
#   src/rkkvm-hid    - userspace USB HID gadget (FunctionFS): keyboard + relative/absolute mouse
#   src/rkkvm-video  - userspace UVC MJPEG streamer + web UI + auth + admin shell + GPIO control
#   src/gpioscan     - GPIO short/jumper scanner (for finding the power-jumper pins)
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TC_VER=2024.05-1
TC=armv7-eabihf--musl--stable-$TC_VER
STAGE="$ROOT/build/stage"
DL="$ROOT/dl"
mkdir -p "$DL" "$ROOT/build" "$STAGE"

# ---------------------------------------------------------------- toolchain
if [ ! -d "$ROOT/toolchain/$TC" ]; then
	echo ">> downloading Bootlin toolchain $TC"
	mkdir -p "$ROOT/toolchain"
	curl -L -o "$ROOT/toolchain/tc.tar.xz" \
	  "https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$TC.tar.xz"
	tar xf "$ROOT/toolchain/tc.tar.xz" -C "$ROOT/toolchain"
fi
export PATH="$ROOT/toolchain/$TC/bin:$PATH"

# ---------------------------------------------------------------- libusb (static)
if [ ! -f "$STAGE/lib/libusb-1.0.a" ]; then
	echo ">> building libusb"
	cd "$DL"
	[ -d libusb-1.0.29 ] || { curl -L -o libusb.tar.bz2 \
	  "https://mirrors.tuna.tsinghua.edu.cn/ubuntu/pool/main/libu/libusb-1.0/libusb-1.0_1.0.29.orig.tar.bz2"; tar xf libusb.tar.bz2; }
	cd libusb-1.0.29
	./configure --host=arm-linux --prefix="$STAGE" \
	  --disable-shared --enable-static --disable-udev \
	  --disable-examples-build --disable-tests-build CC=arm-linux-gcc
	make -j"$(nproc)" && make install
fi

# ---------------------------------------------------------------- libuvc (static)
if [ ! -f "$STAGE/lib/libuvc.a" ]; then
	echo ">> building libuvc"
	cd "$DL"
	[ -d libuvc-0.0.7 ] || { curl -L -o libuvc.tar.gz \
	  "https://codeload.github.com/libuvc/libuvc/tar.gz/refs/tags/v0.0.7"; tar xf libuvc.tar.gz; }
	cd "$ROOT"
	PKG_CONFIG_PATH="$STAGE/lib/pkgconfig" PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig" \
	cmake -S "$DL/libuvc-0.0.7" -B build/libuvc \
	  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake-armv7-musl.cmake" \
	  -DCMAKE_INSTALL_PREFIX="$STAGE" \
	  -DCMAKE_BUILD_TARGET=Static -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF \
	  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=OFF
	cmake --build build/libuvc -j"$(nproc)" && cmake --install build/libuvc
fi

# ---------------------------------------------------------------- libjpeg-turbo (static)
if [ ! -f "$STAGE/lib/libturbojpeg.a" ]; then
	echo ">> building libjpeg-turbo"
	cd "$DL"
	[ -d libjpeg-turbo-3.0.4 ] || { curl -L -o ljt.tar.gz \
	  "https://codeload.github.com/libjpeg-turbo/libjpeg-turbo/tar.gz/refs/tags/3.0.4"; tar xf ljt.tar.gz; }
	cd "$ROOT"
	cmake -S "$DL/libjpeg-turbo-3.0.4" -B build/libjpeg \
	  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake-armv7-musl.cmake" \
	  -DCMAKE_INSTALL_PREFIX="$STAGE" \
	  -DENABLE_SHARED=0 -DENABLE_STATIC=1 -DWITH_TURBOJPEG=1 -DWITH_SIMD=1 -DWITH_JAVA=0
	cmake --build build/libjpeg -j"$(nproc)" && cmake --install build/libjpeg
fi

# ---------------------------------------------------------------- our programs
echo ">> building rk-kvm"
cd "$ROOT/src"
arm-linux-gcc -static -O2 -pthread -o rkkvm-hid hid_ffs.c
arm-linux-gcc -static -O2 -pthread \
  -I"$STAGE/include" -I"$STAGE/include/libusb-1.0" \
  -o rkkvm-video video_uvc.c \
  "$STAGE/lib/libuvc.a" "$STAGE/lib/libusb-1.0.a" "$STAGE/lib/libturbojpeg.a" -lpthread -lm
arm-linux-gcc -static -O2 -o gpioscan gpioscan.c
arm-linux-gcc -static -O2 -I"$STAGE/include" -I"$STAGE/include/libusb-1.0" \
  -o uac_capture uac_capture.c "$STAGE/lib/libusb-1.0.a" -lpthread -lm
arm-linux-gcc -static -O2 -I"$STAGE/include" -I"$STAGE/include/libusb-1.0" \
  -o usbdump usbdump.c "$STAGE/lib/libusb-1.0.a" -lpthread -lm

echo ">> done: src/rkkvm-hid  src/rkkvm-video  src/gpioscan  src/uac_capture  src/usbdump"
