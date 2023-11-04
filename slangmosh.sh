#!/bin/bash

slangmosh --output slangmosh_decode.hpp --output-interface /tmp/dummy.hpp -O --strip Granite/video/slangmosh_decode.json --namespace FFmpegDecode
slangmosh --output slangmosh_encode.hpp --output-interface /tmp/dummy.hpp -O --strip Granite/video/slangmosh_encode.json --namespace FFmpegEncode
slangmosh --output slangmosh_blit.hpp slangmosh_blit.json -O --strip --namespace Blit
