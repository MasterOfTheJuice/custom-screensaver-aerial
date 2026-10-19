#!/bin/sh
# Build bin/aerial-acb for webOS 4. Links the webosbrew media/smp/acb sample
# (libAcbAPI + starfish BUFFERSTREAM) with the openlgtv buildroot-nc4 SDK.
# The sample sources are not vendored; point SAMPLES and SDK at checkouts.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
SAMPLES="${SAMPLES:-/tmp/webosbrew-samples}"
SDK="${SDK:-/home/motj/sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
SYS="$SDK/arm-webos-linux-gnueabi/sysroot"
CC="$SDK/bin/arm-webos-linux-gnueabi-gcc"
CXX="$SDK/bin/arm-webos-linux-gnueabi-g++"
BUILD="${SAMPLES}/build"
CFLAGS="-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DHAVE_SDL_WEBOS=1 \
    -O2 -std=gnu11 -fPIE -Wall -Wextra -pthread \
    -I$SAMPLES/media/smp/common -I$SAMPLES/media/common \
    -I$SYS/usr/include/starfish-media-pipeline -I$SYS/usr/include/SDL2 \
    -I$SYS/usr/include/glib-2.0 -I$SYS/usr/lib/glib-2.0/include \
    -isystem $SYS/usr/include/appswitching-control-block"
"$CC" $CFLAGS -c "$ROOT/native/aerial-acb.c" -o /tmp/aerial-acb.o
"$CC" $CFLAGS -c "$ROOT/native/http_curl.c" -o /tmp/http_curl.o
"$CC" $CFLAGS -c "$ROOT/native/mp4_avc.c" -o /tmp/mp4_avc.o
"$CC" $CFLAGS -c "$ROOT/native/playlist.c" -o /tmp/playlist.o
"$CC" $CFLAGS -c "$ROOT/native/osd_label.c" -o /tmp/osd_label.o
"$CXX" -O2 -pie -pthread -Wl,-rpath,'$ORIGIN' -o "$ROOT/native/aerial-acb" \
    /tmp/aerial-acb.o /tmp/http_curl.o /tmp/mp4_avc.o /tmp/playlist.o /tmp/osd_label.o \
    "$BUILD/media/smp/acb/CMakeFiles/media-smp-acb-webos4.dir/acb_plane.c.o" \
    "$BUILD/media/smp/common/libmedia-smp-common-modern.a" \
    "$BUILD/media/common/libmedia-common-sdl.a" \
    "$BUILD/media/common/libmedia-common.a" \
    -L"$SYS/usr/lib" -lAcbAPI -lplayerAPIs -lstdc++ -lpthread -ldl -lSDL2 \
    -lluna-service2 -lglib-2.0 -lpbnjson_c -ldl
echo "built $ROOT/native/aerial-acb"
