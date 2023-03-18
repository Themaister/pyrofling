/* Copyright (c) 2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "file_handle.hpp"
#include <stdint.h>
#include <stdexcept>
#include <memory>
#include <vector>

namespace PyroFling
{
static constexpr uint32_t MessageEventFlag = 0x80000000u;
enum class MessageType : uint32_t
{
	Void = 0,

	OK = 1,
	ErrorProtocol = 2,
	Error = 3,
	ErrorParameter = 4,

	EchoPayload = 100,
	Device = 101,
	ImageGroup = 102,
	PresentImage = 103,
	AcquireImage = 104 | MessageEventFlag,
	FrameComplete = 105 | MessageEventFlag,
	RetireImage = 106 | MessageEventFlag,

	ClientHello = 200,
	ServerHello = 201
};

class Message
{
public:
	Message(MessageType type, uint64_t serial);
	MessageType get_type() const;
	virtual ~Message() = default;
	void operator=(const Message &) = delete;
	Message(const Message &) = delete;
	uint64_t get_serial() const;

private:
	MessageType type;
	uint64_t serial;
};

struct RawMessagePayload;

enum class ClientIntent : uint32_t
{
	VulkanExternalStream = 1,
	EchoStream = 2,
};

#define WIRE_MESSAGE_BODY_IMPL(Type, ExpectedSize) \
struct Type##Message final : Message { \
    using WireFormat = Internal::Wire::Type; \
    static_assert(sizeof(WireFormat) == ExpectedSize, "Unexpected padding."); \
    static constexpr MessageType msg_type() { return MessageType::Type; } \
    Type##Message(uint64_t serial_, const WireFormat &wire_) : \
        Message(msg_type(), serial_), wire(wire_) \
    {} \
    WireFormat wire; \
}; \
namespace Internal \
{ \
    template <> struct msg_type_from_wire<Internal::Wire::Type> \
	{ \
        static constexpr MessageType value = MessageType::Type; \
	}; \
}

#define SINGLE_FILE_HANDLE_MESSAGE_BODY_IMPL(Type) \
struct Type##Message final : Message { \
    static constexpr MessageType msg_type() { return MessageType::Type; } \
    Type##Message(uint64_t serial_, FileHandle fd_) : \
        Message(msg_type(), serial_), fd(std::move(fd_)) \
    {} \
	FileHandle fd; \
}

#define WIRE_MESSAGE_WITH_FILE_HANDLE_BODY_IMPL(Type, ExpectedSize) \
struct Type##Message final : Message { \
    using WireFormat = Internal::Wire::Type; \
    static_assert(sizeof(WireFormat) == ExpectedSize, "Unexpected padding."); \
    static constexpr MessageType msg_type() { return MessageType::Type; } \
    Type##Message(uint64_t serial_, const WireFormat &wire_, FileHandle fd_) : \
        Message(msg_type(), serial_), wire(wire_), fd(std::move(fd_)) \
    {} \
    WireFormat wire; \
	FileHandle fd; \
}; \
namespace Internal \
{ \
    template <> struct msg_type_from_wire<Internal::Wire::Type> \
	{ \
        static constexpr MessageType value = MessageType::Type; \
	}; \
}

#define WIRE_MESSAGE_WITH_FILE_HANDLES_BODY_IMPL(Type, ExpectedSize) \
struct Type##Message final : Message { \
    using WireFormat = Internal::Wire::Type; \
    static_assert(sizeof(WireFormat) == ExpectedSize, "Unexpected padding."); \
    static constexpr MessageType msg_type() { return MessageType::Type; } \
    Type##Message(uint64_t serial_, const WireFormat &wire_, std::vector<FileHandle> fds_) : \
        Message(msg_type(), serial_), wire(wire_), fds(std::move(fds_)) \
    {} \
    WireFormat wire; \
	std::vector<FileHandle> fds; \
}; \
namespace Internal \
{ \
    template <> struct msg_type_from_wire<Internal::Wire::Type> \
	{ \
        static constexpr MessageType value = MessageType::Type; \
	}; \
}

namespace Internal
{
namespace Wire
{
struct ClientHello
{
	ClientIntent intent;
	char name[256 - sizeof(uint32_t)];
};

struct ServerHello
{
	uint32_t version;
	uint32_t capability[15];
};

struct Device
{
	uint8_t device_uuid[16];
	uint8_t driver_uuid[16];
	uint8_t luid[8];
	uint32_t luid_valid;
};

struct ImageGroup
{
	// Assumptions made:
	// Layers = 1
	// Type = 2D
	// Levels = 1
	uint32_t num_images;
	uint32_t width;
	uint32_t height;
	uint32_t vk_format;
	uint32_t vk_color_space;
	uint32_t vk_image_usage;
	uint32_t vk_image_flags;
	uint32_t vk_external_memory_type; // OPAQUE or DRM modifier. TODO: Figure out how to use DRM modifiers.
	uint32_t vk_num_view_formats;
	uint32_t vk_view_formats[15]; // If MUTABLE and vk_num_formats != 0.
	uint64_t drm_modifier;
};

struct PresentImage
{
	// Serial from image group.
	uint64_t image_group_serial;

	// If period > 0, FIFO semantics.
	// If period == 0, MAILBOX semantics.
	// Image will not be latched until timestamp >= last_timestamp + period.
	// Frames may be skipped if server-side processing skips frames,
	// so it is not true FIFO semantics.
	uint16_t period;

	// Must be [0, VulkanImageGroup::num_images).
	uint16_t index;

	// OPAQUE or something special. Binary semaphores only.
	uint32_t vk_external_semaphore_type;

	// Represents the release barrier that client performs.
	uint32_t vk_old_layout;
	uint32_t vk_new_layout;

	// An ID which is passed back in FrameComplete.
	uint64_t id;
};

struct AcquireImage
{
	// Serial from image group.
	uint64_t image_group_serial;

	// Must be [0, VulkanImageGroup::num_images).
	uint32_t index;

	// OPAQUE or something special. Binary semaphores only.
	// If type is 0, it is an eventfd handle on host timeline.
	uint32_t vk_external_semaphore_type;
};

struct RetireImage
{
	// Serial from image group.
	uint64_t image_group_serial;

	// Must be [0, VulkanImageGroup::num_images).
	uint32_t index;

	uint32_t padding;
};

// Not sure how relevant any of this will be.
enum FrameCompleteFlagBits
{
	// If not set, other bits are considered unknown and client cannot infer any meaningful information.
	FRAME_COMPLETE_VALID_BIT = 1 << 0,

	// Server aims to flip the image directly on-screen.
	FRAME_COMPLETE_DISPLAY_FLIPPING_BIT = 1 << 1,
	// This is the sole visible surface. Generally means full-screen. This may or may not imply flipping.
	// We might not be rendering to a display.
	FRAME_COMPLETE_DISPLAY_PRIMARY_SURFACE_BIT = 1 << 2,
	// The server immediately decided to copy the image to a local buffer and released it immediately to client.
	// Retire event may arrive much later than acquire event.
	// This can happen if the server wants to do post-processing of a buffer.
	FRAME_COMPLETE_CONSUME_EARLY_BLIT_BIT = 1 << 3,
	// Client GPU and server GPU is not the same. Usually implies EARLY_BLIT_BIT as well with sysmem roundtrip.
	FRAME_COMPLETE_CONSUME_CROSS_DEVICE_BIT = 1 << 4,

	// The image was consumed by sampling (composition) rather than flip.
	FRAME_COMPLETE_CONSUME_SAMPLED_BIT = 1 << 5,

	// Aim for presentation before server has observed rendering is complete.
	// The release semaphore will gate forward progress of rendering and may cause server frames to be missed
	// if GPU rendering takes too long.
	FRAME_COMPLETE_CONSUME_EAGER_BIT = 1 << 6,

	// If DISPLAY_FLIPPING_BIT and this is set,
	// indicates that we could flip if we change the tiling mode.
	// If DISPLAY_FLIPPING_BIT is not set and this is set,
	// indicates that the tiling mode is not optimal for sampling / composition.
	FRAME_COMPLETE_SUBOPTIMAL_BIT = 1 << 7,

	// There is no direct correlation between completion and photons hitting a display, so
	// completion event cannot be used to measure latency in a meaningful way.
	// Relevant if the server just encodes video for example or outputs to /dev/null.
	// Present completion does correspond to a GPU processing deadline however ...
	FRAME_COMPLETE_ASYNC_DISPLAY_BIT = 1 << 8,

	// Server would present this image, but the present had no effect on output.
	// Client may choose to back off rendering rate until this flag is unset.
	// The same presentID may be reported as complete at a later time.
	// Client may ignore this and keep rendering as normal.
	FRAME_COMPLETE_DROPPED_BIT = 1 << 9,

	// headroom_ns contains meaningful data.
	FRAME_COMPLETE_HEADROOM_VALID_BIT = 1 << 10,

	// Periods have variable rate. For variable refresh displays.
	// period_ns represents the lowest possible interval.
	FRAME_COMPLETE_VARIABLE_PERIOD_BIT = 1 << 11,
};
using FrameCompleteFlags = uint32_t;

struct FrameComplete
{
	// Serial from image group.
	uint64_t image_group_serial;

	// All processing for timestamp is committed and submitted.
	// Will increase by 1 for every refresh cycle of the server.
	// There may be gaps in the reported timestamp.
	uint64_t timestamp;

	// The current period for frame latches.
	// A new frame complete event is expected after period_ns.
	uint64_t period_ns;

	// When an image is consumed for the first time, it is considered complete.
	uint64_t presented_id;

	FrameCompleteFlags flags;

	// Number of refresh cycles that frame complete was delayed compared to its target timestamp.
	// If this is consistently not zero, the client is too slow.
	uint32_t delayed_count;

	uint64_t headroom_ns;
};
}
}

namespace Internal
{
template<typename TWire>
struct msg_type_from_wire;
}

// Init
WIRE_MESSAGE_BODY_IMPL(ClientHello, 256);
WIRE_MESSAGE_BODY_IMPL(ServerHello, 16 * sizeof(uint32_t));

// Swapchain
WIRE_MESSAGE_BODY_IMPL(Device, 16 * 2 + 8 + 4);
WIRE_MESSAGE_WITH_FILE_HANDLES_BODY_IMPL(ImageGroup, 44 + 15 * 4);
WIRE_MESSAGE_WITH_FILE_HANDLE_BODY_IMPL(PresentImage, 32);
WIRE_MESSAGE_WITH_FILE_HANDLE_BODY_IMPL(AcquireImage, 16);
WIRE_MESSAGE_BODY_IMPL(RetireImage, 16);
WIRE_MESSAGE_BODY_IMPL(FrameComplete, 48);

// Misc
SINGLE_FILE_HANDLE_MESSAGE_BODY_IMPL(EchoPayload);

bool send_message(const FileHandle &fd,
                  MessageType type, uint64_t serial,
                  const void *payload, size_t payload_size,
                  const FileHandle *fling_fds, size_t fling_fds_count);
bool send_message(const FileHandle &fd,
                  MessageType type, uint64_t serial);

template <typename TWireFormat>
static inline bool send_wire_message(const FileHandle &fd, uint64_t serial, const TWireFormat &fmt,
									 const FileHandle *fling_fds = nullptr, size_t fling_fds_count = 0)
{
	return send_message(fd, Internal::msg_type_from_wire<TWireFormat>::value, serial, &fmt, sizeof(fmt),
	                    fling_fds, fling_fds_count);
}

std::unique_ptr<Message> parse_message(const FileHandle &fd);

template <typename MessageT>
static inline MessageT &get(Message &msg)
{
	if (msg.get_type() != MessageT::msg_type())
		throw std::bad_cast();
	return static_cast<MessageT &>(msg);
}

template <typename MessageT>
static inline MessageT &get(const Message &msg)
{
	if (msg.get_type() != MessageT::msg_type())
		throw std::bad_cast();
	return static_cast<const MessageT &>(msg);
}

template <typename MessageT>
static inline MessageT *maybe_get(Message &msg)
{
	if (msg.get_type() == MessageT::msg_type())
		return &static_cast<MessageT &>(msg);
	else
		return nullptr;
}

template <typename MessageT>
static inline const MessageT *maybe_get(const Message &msg)
{
	if (msg.get_type() == MessageT::msg_type())
		return &static_cast<const MessageT &>(msg);
	else
		return nullptr;
}
}