#!/bin/bash
mkdir -p ffmpeg-build-linux-steamrt
cd ffmpeg-build-linux-steamrt

# Steam runtime does not contain nasm/yasm. We don't care about that.
# We just need to access Vulkan video and/or libva.

../ffmpeg/configure --enable-static --disable-shared --prefix="$(pwd)/output" \
	--disable-sdl2 \
	--disable-ffmpeg \
	--disable-ffprobe \
	--disable-x86asm \
	--disable-ffplay

make -j$(nproc) install

