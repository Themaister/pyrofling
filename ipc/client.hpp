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
#include "messages.hpp"
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

namespace PyroFling
{
using SerialHandler = std::function<bool (Message &)>;

class Client
{
public:
	explicit Client(const char *name);

	uint64_t send_message_raw(MessageType type, const void *payload, size_t payload_size,
	                          const FileHandle *fling_fds = nullptr, size_t fling_fds_count = 0);

	template <typename TWireFormat>
	inline uint64_t send_wire_message(const TWireFormat &wire, const FileHandle *fling_fds = nullptr, size_t fling_fds_count = 0)
	{
		return send_message_raw(Internal::msg_type_from_wire<TWireFormat>::value, &wire, sizeof(wire),
								fling_fds, fling_fds_count);
	}

	inline uint64_t send_file_handle_message(MessageType type, const FileHandle &fd_)
	{
		return send_message_raw(type, nullptr, 0, &fd_, 1);
	}

	// Not thread-safe if there are concurrent threads reading from connection, unless lock is taken.
	void set_serial_handler(uint64_t serial, SerialHandler func);
	void set_default_serial_handler(SerialHandler func);
	void set_event_handler(SerialHandler func);

	const FileHandle &get_file_handle() const;

	// Cooperative reading of connection and event handling.
	// Any thread calling these may call serial handlers and event handlers.
	bool roundtrip(std::unique_lock<std::mutex> &lock);
	int wait_reply(std::unique_lock<std::mutex> &lock, int timeout_ms = -1);
	bool wait_reply_for_serial(std::unique_lock<std::mutex> &lock, uint64_t serial);
	MessageType wait_plain_reply_for_serial(std::unique_lock<std::mutex> &lock, uint64_t serial);

private:
	FileHandle fd;
	uint64_t send_serial = 0;
	uint64_t received_replies = 0;
	std::unordered_map<uint64_t, SerialHandler> handlers;
	SerialHandler default_handler;
	SerialHandler event_handler;

	bool process();

	std::condition_variable read_cond;
	bool has_socket_master = false;
	bool socket_master_error = false;
	uint64_t process_count = 0;
};
}