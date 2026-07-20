#!/bin/bash

mkdir -p ffmpeg-build-linux-steamrt
cd ffmpeg-build-linux-steamrt

# The glslangValidator in the steamrt image is too old and fails to compile the shaders in libavcodec.
# There is no easy and obvious way to disable only the glslc path,
# but if all the compilers fail, it will disable glslc for us.
export PATH=".:$PATH"
ln -sf /bin/false glslc
ln -sf /bin/false glslangValidator
ln -sf /bin/false glslang

# Steam runtime does not contain nasm/yasm. We don't care about that.
# We just need to access Vulkan video and/or libva.
# The image ships SPIR-V headers, but they're too old. Override them.
../ffmpeg/configure --enable-static --disable-shared --prefix="$(pwd)/output" \
	--disable-sdl2 \
	--extra-cflags="-I/pyro/Granite/third_party/khronos/vulkan-headers/include -I/pyro/Granite/third_party/spirv-headers/include" \
	--disable-ffmpeg \
	--disable-ffprobe \
	--disable-x86asm \
	--enable-vulkan \
	--disable-ffplay

make -j$(nproc) install

