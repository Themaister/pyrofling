#!/bin/bash

./Granite/tools/create_android_build.py \
	--output-gradle android \
	--application-id net.themaister.pyrofling_viewer \
	--granite-dir Granite \
	--native-target pyrofling-viewer \
	--app-name "PyroFling Viewer" \
	--abis arm64-v8a \
	--cmake-lists-toplevel CMakeLists.txt \
	--assets assets \
	--builtin assets \
	--audio
echo org.gradle.jvmargs=-Xmx4096M >> gradle.properties
