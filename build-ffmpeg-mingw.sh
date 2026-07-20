#!/bin/bash
export CFLAGS="-static -static-libgcc -static-libstdc++"

mkdir -p ffmpeg-build-msys2
cd ffmpeg-build-msys2

../ffmpeg/configure --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- \
	--enable-static --disable-shared --prefix="$(pwd)/output" \
	--disable-sdl2 \
	--enable-vulkan \
	--disable-vaapi \
	--disable-cuda \
	--disable-bzlib \
	--disable-iconv \
	--disable-lzma \
	--disable-zlib \
	--disable-ffmpeg \
	--disable-ffprobe \
	--disable-ffplay

make -j$(nproc) install

