# PyroFling

PyroFling is a simple solution for capturing Vulkan applications and
broadcast video and audio to streaming platforms using FFmpeg, PyroEnc and PyroWave.
The recording itself is implemented as a separate process which acts like a very basic compositor.

## Building

Normal CMake. Granite submodule must also be initialized.

```shell
git submodule update --init --recursive
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local -G Ninja
ninja install
```

### Android

Android build is a little more elaborate. First, FFmpeg need to be built with:

```shell
bash ./build-ffmpeg-android.sh
```

See script for how `ANDROID_SDK_HOME` and `ANDROID_NDK_HOME` should be set.
It assumes the SDK and NDK is installed.

Once FFmpeg libraries are built:

```shell
bash ./build-android.sh
```

Which generates a Gradle project and invokes Gradle.
APK is in `./android/build/outputs/apk/$buildtype`.

## IPC

`pyrofling-ipc` implements a basic Unix domain socket setup where client can send messages and receive replies
asynchronously. File descriptors are sent between client and server with OPAQUE_FD handles created by Vulkan.

### Explicit sync

All synchronization between client and server is explicit sync using semaphores.

## Layer

The recording process and Vulkan applications communicate with each other through the PyroFling layer.
The layer is controlled with environment variables.

### `PYROFLING=1`

Enables the layer. If a server is active, the connection is automatic. If a server is not currently active,
the layer will attempt to reconnect at an interval, so it is feasible to start a game first, and then start the
server. The server can be respawned and the layer will automatically connect to the new process.

### `PYROFLING_SERVER=path`

By default, `/tmp/pyrofling-socket` is used, but can be overridden if need be.

### `PYROFLING_SYNC=mode`

By default, the pyrofling layer will lock its presentation loop to a heartbeat of the server
if the application is using FIFO presentation modes.
This ensures proper frame pacing for encoded output. This is optimal for a VRR display setup where
the display will then end up locking to the heartbeat of the server elegantly.
If application uses IMMEDIATE or MAILBOX presentation modes, the layer will not lock the presentation
loop to server, unless overridden.

#### `default`

As explained above.

#### `client`

Encoding loop is completely unlocked and server will sample
the last ready image every heartbeat cycle. This leads to more judder in the encoded video.
Client will render at whatever frame rate it would otherwise do.
E.g. it might render at 144 fps while encoding happens at 60 fps.

#### `server`

Encoding loop is forced to be locked to server rate, but the swapchain's present mode
is forced to MAILBOX. This ensures optimal frame pacing for encoded video,
but poor pacing for the swapchain itself. This is useful when doing e.g. remote play where
the pacing of the local display is irrelevant, and the local display does not support VRR or similar.

### `PYROFLING_IMAGES=n`

Forces a certain swap chain depth between client and server. Used to improve latency in real-time scenarios.
When `--immediate-encode` is used on server side, adding more images does not add more end-to-end latency,
but will affect how rapidly the client can react to rate changes, and various other subtle things.
Should generally be left alone.

## Server

```
[INFO]: Usage: pyrofling
    [--socket SOCKET_PATH]
    [--width WIDTH]
    [--height HEIGHT]
    [--fps FPS]
    [--device-index INDEX]
    [--client-rate-multiplier RATE]
    [--threads THREADS]
    [--preset PRESET]
    [--tune PRESET]
    [--gop-seconds GOP_SECONDS (negative for IDR-on-demand mode if intra-refresh is not supported)]
    [--bitrate-kbits SIZE]
    [--max-bitrate-kbits SIZE]
    [--vbv-size-kbits SIZE]
    [--local-backup PATH]
    [--encoder ENCODER]
    [--muxer MUXER]
    [--port PORT]
    [--audio-rate RATE]
    [--low-latency]
    [--no-audio]
    [--immediate-encode]
    url
```

### Peer-to-peer game streaming with low latency

#### --encoder

`h264_pyro`, `h265_pyro` and `pyrowave` (my own intra-only codec) are good for this.
For the H.264 and H.265 codecs, Vulkan video support is required.

```shell
$ pyrofling --encoder h264_pyro --width 1920 --height 1080 --bitrate-kbits 50000 --immediate-encode --port 9000 --fps 60 --low-latency --gop-seconds -1
$ pyrofling --encoder pyrowave --width 1920 --height 1080 --bitrate-kbits 250000 --immediate-encode --port 9000 --fps 60 --low-latency
```

```shell
PYROFLING=1 PYROFLING_SYNC=server somevulkanapp
```

Negative GOP seconds means infinite GOP. IDR frames are automatically sent on packet losses.

#### --fec

Forward error correction can be added with `--fec`. This adds about 25% more network bandwidth on top,
and can help correct errors.

The server reports statistics every second, e.g.

```
PROGRESS for localhost @ 44860: 31539 complete, 129 dropped video, 13 dropped audio, 259 key frames, 500 FEC recovered.
```

This can be used to eye-ball the health of the connection.

#### HDR

There is experimental HDR encoding supported. Add `--hdr10` to server which will transmit video in BT.2020 / PQ instead of BT.709.
When capturing games, especially on Proton, this might be needed when running the game:

```
PYROFLING_FORCE_VK_COLOR_SPACE=HDR10 DXVK_HDR=1 PYROFLING=1 PYROFLING_SYNC=server %command%
```

`scRGB` is also supported if game wants to use that. This workaround is only needed
when the WSI of the machine running the game does not natively support HDR WSI.
Once Proton moves over to Wayland fully, this is less likely to be an issue.

#### 4:4:4 chroma subsampling

Especially when encoding HDR, 4:4:4 chroma subsampling seems very important.
This is currently only supported for PyroWave and non-GPU accelerated codecs in FFmpeg. GPU accelerated H.264 / H.265 is TBD.

#### Client side

For VRR displays, or where absolute minimal latency (at cost of jitter) is desired:

```shell
pyrofling-viewer "pyro://<ip>:<port>"
```

Replace `<ip>` and `<port>` as desired.

For frace-paced output:

This aims to receive the next frame at 0.0 seconds offset after previous vblank is complete,
giving us ~16ms from when packets are received until they reach display for 60 FPS.
Deadline at 0.008 means that if we don't receive a frame until 0.008, assume it's not arriving,
or attempt to recover a damaged frame if there has been some packet loss, especially with pyrowave
since it's robust against errors.

```shell
pyrofling-viewer "pyro://<ip>:<port>?phase_locked=0.0&deadline=0.008"
```

##### Android (very WIP)

Android is currently a big hack as there is no easy way to provide a command line,
and keyboard input support is limited.
For now, create a default command line text file, e.g. create `uri.txt`:

```
pyro://10.0.0.2:9000
```

and then push it to device with:

```
adb push uri.txt /data/local/tmp/pyrofling-uri.txt
```

Once APK starts, it should prompt for "enter", touch the screen to connect.

#### Client stats window

While client is live, press 'V' key to display a HUD.
Gamepad inputs registered by the client will show up as a green box in the top-left corner.

#### Gamepad hand-off

To take control with a game-pad, the "mode" button can be used.
E.g. on a PS4 or PS5 controller, that's the PlayStation button.
On controllers where this does not exist, like the Steam Deck, press Start + Select + L1 + R1.
This way, multiple clients can do virtual couch coop with little to no friction.

##### Connecting purely as a gamepad client

This can be useful when multiple people want to share the same virtual controller (for e.g. virtual couch coop).
The player using the mode button takes control until someone else takes control.

```shell
pyrofling-gamepad "<ip>:<port>"
```

### Streaming to Twitch example (1080p60)

```
pyrofling "rtmp://live.twitch.tv/app/live_*_*" \ # replace with your stream key
	--local-backup ~/pyrofling.mkv \
	--width 1920 --height 1080 \
	--bitrate-kbits 5500 \
	--vbv-size-kbits 5000 \
	--fps 60 \
	--preset fast --threads 16

PYROFLING=1 somevulkanapp
```

Other RTMP setups is basically the same, just different URLs.

#### Misc tweaks

E.g.:

`sudo tc qdisc add dev $iface root fq maxrate 100000000 flow_limit 2000 quantum 2048`

to limit outgoing traffic to 100 mbit on Linux.
Can be useful when doing real-time streaming over long distance.
NOTE: Do not use this when streaming with pyrowave, it will only increase latency.

### Special features

#### Client rate multiplier

If encode is e.g. 60 fps, it's possible to use rate multiplier to make the heartbeat twice as fast.
This way, the client can play at smooth 120 fps while a half-rate stream is served over the network.

#### Local backup

The RTMP stream can be muxed into a local file for reference. No additional encoding is performed.

#### Cross-device support

A client and server can be different GPUs.
In this case, the image will roundtrip through system memory
with `VK_EXT_external_memory_host` if available.

### Audio recording

On server startup, a recording stream is created against the default input. In Pipewire, programs such as
`qpwgraph` can be used to redirect a specific application's audio output into that stream.
Alternatively, it's possible to redirect the monitor stream of an audio output as well.

### Composition

Currently, the server just encodes one of the clients' images by scaling it to the encode resolution.
In theory, it would be possible to add various overlays (ala OBS) should the need arise.

### Security

There is none at this time. Use at your own risk and do not transmit anything sensitive. Use at your own risk.

## License

MIT for PyroFling code. Note that linking against FFmpeg (GPL-enabled build)
may affect license of PyroFling if distributed.
