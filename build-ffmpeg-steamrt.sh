#!/bin/bash

docker run -it --init --rm -u $UID -w /pyro -v $(pwd):/pyro registry.gitlab.steamos.cloud/steamrt/sniper/sdk ./build-ffmpeg-steamrt-helper.sh

