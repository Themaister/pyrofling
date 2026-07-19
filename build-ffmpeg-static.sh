#!/bin/bash
mkdir -p ffmpeg-build-static
cd ffmpeg-build-static

../ffmpeg/configure --enable-static --disable-shared --prefix="$(pwd)/output" \
	--disable-sdl2 \
	--disable-ffmpeg \
	--disable-ffprobe \
	--disable-ffplay \
	--enable-vulkan

make -j$(nproc) install

