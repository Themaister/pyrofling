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

#include "client.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <stdexcept>
#include <string.h>
#include <assert.h>

namespace PyroFling
{
Client::Client(const char *name)
{
	fd = FileHandle(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0));
	if (!fd)
		throw std::runtime_error("Failed to create socket.");

	if (!name)
		throw std::invalid_argument("name is nullptr");

	struct sockaddr_un sock_addr = {};
	sock_addr.sun_family = AF_UNIX;
	strncpy(sock_addr.sun_path, name, sizeof(sock_addr.sun_path) - 1);

	if (connect(fd.get_native_handle(),
	            reinterpret_cast<const sockaddr *>(&sock_addr),
	            sizeof(sock_addr)) < 0)
	{
		throw std::runtime_error("Failed to connect.");
	}
}

const FileHandle &Client::get_file_handle() const
{
	return fd;
}

void Client::set_serial_handler(uint64_t serial, SerialHandler func)
{
	assert(serial != 0);
	handlers[serial] = std::move(func);
}

void Client::set_default_serial_handler(SerialHandler func)
{
	default_handler = std::move(func);
}

void Client::set_event_handler(SerialHandler func)
{
	event_handler = std::move(func);
}

uint64_t Client::send_message_raw(MessageType type, const void *payload, size_t payload_size,
                                  const FileHandle *fling_fds, size_t fling_fds_count)
{
	struct pollfd pfd = {};
	pfd.fd = fd.get_native_handle();
	pfd.events = POLLOUT;

	if (::poll(&pfd, 1, 1000) <= 0 || (pfd.revents & POLLOUT) == 0)
	{
		fprintf(stderr, "Connection is congested, server is likely hung.\n");
		return 0;
	}

	if (send_message(fd, type, send_serial + 1, payload, payload_size, fling_fds, fling_fds_count))
	{
		send_serial += 1;
		return send_serial;
	}
	else
		return 0;
}

int Client::wait_reply(int timeout_ms)
{
	struct pollfd pfd = {};
	pfd.fd = fd.get_native_handle();
	pfd.events = POLLIN;
	int ret;

	if ((ret = ::poll(&pfd, 1, timeout_ms)) <= 0)
		return ret;

	if ((pfd.revents & POLLIN) == 0)
		return 0;

	return process() ? 1 : -1;
}

bool Client::roundtrip()
{
	while (received_replies < send_serial)
		if (wait_reply() <= 0)
			return false;

	return true;
}

bool Client::wait_reply_for_serial(uint64_t serial)
{
	while (received_replies < serial)
	{
		if (wait_reply() <= 0)
			return false;
	}

	return true;
}

MessageType Client::wait_plain_reply_for_serial(uint64_t serial)
{
	MessageType type = MessageType::Void;
	if (!serial)
		return type;

	set_serial_handler(serial, [&type](const Message &msg) {
		type = msg.get_type();
		return true;
	});

	if (!wait_reply_for_serial(serial))
		return MessageType::Void;
	return type;
}

bool Client::process()
{
	auto msg = parse_message(fd);
	if (!msg)
		return false;

	// Serial 0 is for out of band async events that server will notify us out of band.
	if (msg->get_serial() == 0)
	{
		if ((uint32_t(msg->get_type()) & MessageEventFlag) == 0)
		{
			fprintf(stderr, "Unexpected message type #%x, event flag not set.\n", uint32_t(msg->get_type()));
			return false;
		}

		if (event_handler && !event_handler(*msg))
			return false;

		return true;
	}

	if ((uint32_t(msg->get_type()) & MessageEventFlag) != 0)
	{
		fprintf(stderr, "Unexpected message type #%x, event flag is unexpectedly set.\n", uint32_t(msg->get_type()));
		return false;
	}

	// Otherwise, serial values must be replied in order, effectively RPC.

	received_replies++;
	if (msg->get_serial() != received_replies)
	{
		fprintf(stderr, "Unexpected serial, expected %llu, got %llu.\n",
		        static_cast<unsigned long long>(received_replies),
		        static_cast<unsigned long long>(msg->get_serial()));
		return false;
	}

	auto itr = handlers.find(msg->get_serial());
	if (itr != handlers.end())
	{
		if (itr->second && !itr->second(*msg))
			return false;
		handlers.erase(itr);
	}
	else if (default_handler && !default_handler(*msg))
		return false;

	return true;
}
}