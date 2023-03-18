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

#include "messages.hpp"
#include "file_handle.hpp"
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace PyroFling
{
static constexpr uint64_t Magic = 0x7538244abd122f9full;

struct RawMessageHeader
{
	uint64_t magic;
	uint64_t serial;
	MessageType type;
	uint32_t payload_len;
	uint64_t reserved;
};
static_assert(sizeof(RawMessageHeader) == 32, "Unexpected header size.");

struct RawMessagePayload
{
	RawMessageHeader msg;
	alignas(max_align_t) uint8_t data[1024 - sizeof(msg)];
};
static_assert(sizeof(RawMessagePayload) == 1024, "Unexpected payload size.");

MessageType Message::get_type() const
{
	return type;
}

Message::Message(MessageType type_, uint64_t serial_)
	: type(type_), serial(serial_)
{
}

uint64_t Message::get_serial() const
{
	return serial;
}

static constexpr int MaxSockets = 16;

bool send_message(const FileHandle &fd,
                  MessageType type, uint64_t serial,
                  const void *payload, size_t payload_size,
                  const FileHandle *fling_fds, size_t fling_fds_count)
{
	RawMessageHeader header = {};
	iovec iovs[2] = {};
	int iov_count = 0;

	if (fling_fds_count > size_t(MaxSockets))
		return false;

	if (payload_size > sizeof(RawMessagePayload::data))
		return false;

	alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int) * MaxSockets)];

	header.magic = Magic;
	header.type = type;
	header.payload_len = payload_size;
	header.serial = serial;

	iovs[iov_count].iov_base = &header;
	iovs[iov_count].iov_len = sizeof(header);
	iov_count++;

	if (payload_size)
	{
		iovs[iov_count].iov_base = const_cast<void *>(payload);
		iovs[iov_count].iov_len = payload_size;
		iov_count++;
	}

	msghdr msg = {};
	msg.msg_iov = iovs;
	msg.msg_iovlen = iov_count;

	if (fling_fds_count)
	{
		msg.msg_control = cmsg_buf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * fling_fds_count);

		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fling_fds_count);
		auto *fds = reinterpret_cast<int *>(CMSG_DATA(cmsg));
		for (size_t i = 0; i < fling_fds_count; i++)
			fds[i] = fling_fds[i].get_native_handle();
	}

	ssize_t ret = ::sendmsg(fd.get_native_handle(), &msg, MSG_NOSIGNAL);

	if (ret == -1 && errno == EAGAIN)
		fprintf(stderr, "Non-blocking write fail. Clogged pipes somewhere?\n");

	return size_t(ret) == sizeof(header) + payload_size;
}

bool send_message(const FileHandle &fd, MessageType type, uint64_t serial)
{
	return send_message(fd, type, serial, nullptr, 0, nullptr, 0);
}

template <typename T>
static inline std::unique_ptr<Message> create_single_file_handle_message(const RawMessagePayload &payload,
																		 std::vector<FileHandle> handles)
{
	if (payload.msg.payload_len != 0)
	{
		fprintf(stderr, "Message type %u: expected wire format size 0, got %zu.\n",
		        unsigned(payload.msg.type), size_t(payload.msg.payload_len));
		return {};
	}

	if (handles.size() != 1)
	{
		fprintf(stderr, "Expected 1 file handle, got %zu.\n", handles.size());
		return {};
	}

	return std::make_unique<T>(payload.msg.serial, std::move(handles.front()));
}

template <typename T>
static inline std::unique_ptr<Message> create_file_handles_wire_format_message(const RawMessagePayload &payload,
                                                                               std::vector<FileHandle> handles)
{
	return std::make_unique<T>(payload.msg.serial,
	                           *reinterpret_cast<const typename T::WireFormat *>(payload.data),
	                           std::move(handles));
}

template <typename T>
static inline std::unique_ptr<Message> create_file_handle_wire_format_message(const RawMessagePayload &payload,
                                                                              std::vector<FileHandle> handles)
{
	if (handles.size() > 1)
	{
		fprintf(stderr, "Expected 0 or 1 file handle, got %zu.\n", handles.size());
		return {};
	}

	FileHandle fd;
	if (handles.size() == 1)
		fd = std::move(handles.front());

	return std::make_unique<T>(payload.msg.serial,
	                           *reinterpret_cast<const typename T::WireFormat *>(payload.data),
	                           std::move(fd));
}

template <typename T>
static inline std::unique_ptr<Message> create_wire_format_message(const RawMessagePayload &payload)
{
	if (payload.msg.payload_len != sizeof(typename T::WireFormat))
	{
		fprintf(stderr, "Message type %u: expected wire format size %zu, got %zu.\n",
		        unsigned(payload.msg.type),
		        sizeof(typename T::WireFormat), size_t(payload.msg.payload_len));
		return {};
	}

	return std::make_unique<T>(payload.msg.serial,
							   *reinterpret_cast<const typename T::WireFormat *>(payload.data));
}

static std::unique_ptr<Message> decode_message(const RawMessagePayload &payload, std::vector<FileHandle> &received_fds)
{
	std::unique_ptr<Message> result;
	switch (payload.msg.type)
	{
	case MessageType::EchoPayload:
		result = create_single_file_handle_message<EchoPayloadMessage>(payload, std::move(received_fds));
		break;

	case MessageType::OK:
	case MessageType::ErrorProtocol:
	case MessageType::Error:
	case MessageType::ErrorParameter:
		result = std::make_unique<Message>(payload.msg.type, payload.msg.serial);
		break;

	case MessageType::ClientHello:
		result = create_wire_format_message<ClientHelloMessage>(payload);
		break;

	case MessageType::ServerHello:
		result = create_wire_format_message<ServerHelloMessage>(payload);
		break;

	case MessageType::Device:
		result = create_wire_format_message<DeviceMessage>(payload);
		break;

	case MessageType::ImageGroup:
		result = create_file_handles_wire_format_message<ImageGroupMessage>(payload, std::move(received_fds));
		break;

	case MessageType::PresentImage:
		result = create_file_handle_wire_format_message<PresentImageMessage>(payload, std::move(received_fds));
		break;

	case MessageType::AcquireImage:
		result = create_file_handle_wire_format_message<AcquireImageMessage>(payload, std::move(received_fds));
		break;

	case MessageType::FrameComplete:
		result = create_wire_format_message<FrameCompleteMessage>(payload);
		break;

	case MessageType::RetireImage:
		result = create_wire_format_message<RetireImageMessage>(payload);
		break;

	default:
		fprintf(stderr, "Unexpected message.\n");
		return {};
	}

	return result;
}

std::unique_ptr<Message> parse_message(const FileHandle &fd)
{
	RawMessagePayload payload = {};
	iovec iov = {};

	alignas(struct cmsghdr) char cmsg_buf[CMSG_SPACE(sizeof(int) * MaxSockets)];

	iov.iov_base = &payload;
	iov.iov_len = sizeof(payload);

	msghdr msg = {};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	ssize_t ret = ::recvmsg(fd.get_native_handle(), &msg, 0);

	if (ret <= 0)
		return {};

	// Capture any FDs we receive.
	std::vector<FileHandle> received_fds;

	for (auto *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
	{
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && cmsg->cmsg_len > CMSG_LEN(0))
		{
			size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
			size_t num_fds = data_len / sizeof(int);
			auto *fds = reinterpret_cast<const int *>(CMSG_DATA(cmsg));
			for (size_t i = 0; i < num_fds; i++)
				received_fds.emplace_back(fds[i]);
		}
	}

	if (payload.msg.magic != Magic)
	{
		fprintf(stderr, "Magic mismatch.\n");
		return {};
	}

	if (size_t(ret) != sizeof(payload.msg) + payload.msg.payload_len)
	{
		fprintf(stderr, "Message length mismatch.\n");
		return {};
	}

	if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
	{
		fprintf(stderr, "Unexpected truncation.\n");
		return {};
	}

	return decode_message(payload, received_fds);
}
}