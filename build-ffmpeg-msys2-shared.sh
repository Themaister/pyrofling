#!/bin/bash

mkdir -p ffmpeg-build-msys2-shared
cd ffmpeg-build-msys2-shared

../ffmpeg/configure --disable-static --enable-shared --prefix=output \
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

cd output/lib

# Generate MSVC import libraries.
for file in *.def
do
	dlltool -d $file -l ${file%-*}.lib -D ${file%.def}.dll
done
