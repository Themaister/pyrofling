#!/bin/bash

if [ ! -d ffmpeg-android-maker ]; then
	git clone https://github.com/Javernaut/ffmpeg-android-maker
fi

if [ -z $ANDROID_SDK_HOME ]; then
	export ANDROID_SDK_HOME="$HOME/Android"
fi

if [ -z $ANDROID_NDK_HOME ]; then
	export ANDROID_NDK_HOME="$ANDROID_SDK_HOME/ndk/26.3.11579264"
fi

echo "ANDROID_SDK_HOME = $ANDROID_SDK_HOME"
echo "ANDROID_NDK_HOME = $ANDROID_NDK_HOME"

cd ffmpeg-android-maker
git pull origin master
bash ./ffmpeg-android-maker.sh -abis=arm64-v8a
