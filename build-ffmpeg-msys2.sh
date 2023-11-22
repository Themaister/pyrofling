#!/bin/bash
export CFLAGS="-static -static-libgcc -static-libstdc++"

mkdir -p ffmpeg-build-msys2
cd ffmpeg-build-msys2

../ffmpeg/configure --enable-static --disable-shared --prefix=output \
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

