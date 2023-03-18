# PyroFling

PyroFling is a simple solution for capturing Vulkan applications and
broadcast video and audio to streaming platforms using FFmpeg.
The recording itself is implemented as a separate process which acts like a very basic compositor.

The end-goal is to use Vulkan video encoding to get hardware accelerated encode.
For now, x264 is used.

## Building

Normal CMake. Granite submodule must also be initialized.

```
git submodule update --init --recursive
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local -G Ninja
ninja install
```

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

### `PYROFLING_FORCE_MAILBOX=1`

By default, the pyrofling layer will lock its presentation loop to a heartbeat of the server
if the application is using FIFO presentation modes.
This ensures proper frame pacing for encoded output. This is optimal for a VRR display setup where
the display will then end up locking to the heartbeat of the server elegantly.

If this environment variable is set however, the heartbeat from server is ignored and rendering will lock to
local display refresh rate, meaning the encoded output will be somewhat more jittery.

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
    [--gop-seconds GOP_SECONDS]
    [--bitrate-kbits SIZE]
    [--max-bitrate-kbits SIZE]
    [--vbv-size-kbits SIZE]
    [--local-backup PATH]
    url
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

## License

MIT for PyroFling code. Note that linking against FFmpeg (GPL-enabled build)
may affect license of PyroFling if distributed.