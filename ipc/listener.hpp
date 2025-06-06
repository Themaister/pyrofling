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
#include <vector>
#include <memory>
#include <string>
#include <stdint.h>
#include <sys/socket.h>

namespace PyroFling
{
class Dispatcher;
class Handler
{
public:
	virtual ~Handler() = default;
	explicit Handler(Dispatcher &dispatcher);
	bool is_sentinel_file_handle() const;
	virtual bool handle(const FileHandle &fd, uint32_t id) = 0;
	virtual void release_id(uint32_t id) = 0;

protected:
	void set_sentinel_file_handle();
	Dispatcher &dispatcher;

private:
	bool is_sentinel = false;
};

class Listener
{
public:
	explicit Listener(const char *name);
	const FileHandle &get_file_handle() const;
	~Listener();

private:
	FileHandle fd;
	std::string unlink_path;
};

struct RemoteAddress
{
	sockaddr_storage addr = {};
	socklen_t addr_size = {};

	bool operator==(const RemoteAddress &other) const;
	bool operator!=(const RemoteAddress &other) const;
	explicit operator bool() const;
};

struct TCPConnection
{
	FileHandle fd;
	RemoteAddress addr;
};

class IPListener
{
public:
	enum class Proto { TCP, UDP };
	IPListener() = default;
	IPListener(Proto proto, const char *port);
	const FileHandle &get_file_handle() const;
	IPListener &operator=(IPListener &&other) = default;
	IPListener(IPListener &&other) = default;

	TCPConnection accept_tcp_connection();
	int read_udp_datagram(RemoteAddress &remote, void *data, unsigned size);

private:
	FileHandle fd;
};

class HandlerFactoryInterface
{
public:
	virtual bool register_handler(Dispatcher &dispatcher, const FileHandle &fd, Handler *&handler) = 0;
	virtual bool register_tcp_handler(Dispatcher &dispatcher, const FileHandle &fd, const RemoteAddress &remote, Handler *&handler) = 0;
	virtual void handle_udp_datagram(Dispatcher &dispatcher, const RemoteAddress &remote, const void *msg, unsigned size) = 0;
};

class Dispatcher
{
public:
	explicit Dispatcher(const char *name, const char *listen_port);
	void set_handler_factory_interface(HandlerFactoryInterface *iface);
	bool iterate();
	void kill();

	static void block_signals();

	enum class ConnectionType
	{
		Input,
		Output,
		InOut
	};
	bool add_connection(FileHandle fd, Handler *handler, uint32_t id, ConnectionType type);
	void cancel_connection(Handler *handler, uint32_t id);
	int write_udp_datagram(const RemoteAddress &addr,
	                       const void *header, unsigned header_size,
	                       const void *data, unsigned size);

private:
	HandlerFactoryInterface *iface = nullptr;
	Listener listener;
	IPListener tcp_listener, udp_listener;
	FileHandle pollfd;

	FileHandle accept_connection();
	TCPConnection accept_tcp_connection();
	void add_signalfd();
	void add_eventfd();
	const FileHandle *event_handle = nullptr;

	struct Connection
	{
		FileHandle fd;
		RemoteAddress remote;
		uint32_t id = 0;
		Handler *handler = nullptr;

		Connection() = default;
		void operator=(const Connection &) = delete;
		Connection(const Connection &) = delete;
		~Connection()
		{
			if (handler)
				handler->release_id(id);
		}
	};

	std::vector<std::unique_ptr<Connection>> connections;
	std::vector<std::unique_ptr<Connection>> cancellations;
	bool iterate_inner();
};
}
