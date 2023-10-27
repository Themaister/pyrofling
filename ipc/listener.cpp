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

#include "listener.hpp"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdexcept>
#include <algorithm>
#include <errno.h>
#include <assert.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

namespace PyroFling
{
Handler::Handler(Dispatcher &dispatcher_)
	: dispatcher(dispatcher_)
{
}

void Handler::set_sentinel_file_handle()
{
	is_sentinel = true;
}

bool Handler::is_sentinel_file_handle() const
{
	return is_sentinel;
}

Listener::~Listener()
{
	unlink(unlink_path.c_str());
}

Listener::Listener(const char *name)
{
	fd = FileHandle(socket(AF_UNIX, SOCK_SEQPACKET, 0));
	if (!fd)
		throw std::runtime_error("Failed to create domain socket.");

	struct stat s = {};
	if (stat(name, &s) >= 0)
	{
		if ((s.st_mode & S_IFMT) == S_IFSOCK)
		{
			fprintf(stderr, "Rebinding socket.\n");
			unlink(name);
		}
	}

	struct sockaddr_un addr_unix = {};
	addr_unix.sun_family = AF_UNIX;
	strncpy(addr_unix.sun_path, name, sizeof(addr_unix.sun_path) - 1);
	int ret = ::bind(fd.get_native_handle(), reinterpret_cast<const sockaddr *>(&addr_unix), sizeof(addr_unix));
	if (ret < 0)
		throw std::runtime_error("Failed to bind socket.");

	unlink_path = name;
}

const FileHandle &Listener::get_file_handle() const
{
	return fd;
}

IPListener::IPListener(Proto proto, const char *port)
{
	addrinfo hints = {};
	addrinfo *res = nullptr;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = proto == Proto::TCP ? SOCK_STREAM : SOCK_DGRAM;
	hints.ai_protocol = proto == Proto::TCP ? IPPROTO_TCP : IPPROTO_UDP;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(nullptr, port, &hints, &res) < 0 || !res)
		throw std::runtime_error("Failed to call getaddrinfo.");

	struct Deleter
	{
		void operator()(addrinfo *info)
		{
			if (info)
				freeaddrinfo(info);
		}
	};
	std::unique_ptr<addrinfo, Deleter> holder{res};

	fd = FileHandle(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
	if (!fd)
		throw std::runtime_error("Failed to create TCP socket.");

	int yes = 1;
	if (setsockopt(fd.get_native_handle(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
		throw std::runtime_error("Failed to set reuseaddr.");

	if (bind(fd.get_native_handle(), res->ai_addr, res->ai_addrlen) < 0)
		throw std::runtime_error("Failed to bind.");

#if 0
	if (proto == Proto::UDP)
	{
		// Keep the sndbuf healthy so we don't block in steady case.
		int size = 4 * 1024 * 1024;
		if (setsockopt(fd.get_native_handle(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0)
			throw std::runtime_error("Failed to set reuseaddr.");

		// Non-blocking as well to avoid stalling encoder. ? Seems to cause issues.
		fcntl(fd.get_native_handle(), F_SETFL, fcntl(fd.get_native_handle(), F_GETFL) | O_NONBLOCK);
	}
#endif
}

int IPListener::read_udp_datagram(RemoteAddress &remote, void *data, unsigned size)
{
	remote.addr_size = sizeof(remote.addr);
	ssize_t ret = ::recvfrom(fd.get_native_handle(), data, size, 0,
	                         reinterpret_cast<sockaddr *>(&remote.addr), &remote.addr_size);
	return int(ret);
}

TCPConnection IPListener::accept_tcp_connection()
{
	TCPConnection conn;
	conn.addr.addr_size = sizeof(conn.addr.addr);
	conn.fd = FileHandle{::accept(fd.get_native_handle(),
	                              reinterpret_cast<sockaddr *>(&conn.addr.addr), &conn.addr.addr_size)};
	return conn;
}

const FileHandle &IPListener::get_file_handle() const
{
	return fd;
}

FileHandle Dispatcher::accept_connection()
{
	FileHandle fd{::accept4(listener.get_file_handle().get_native_handle(),
	                        nullptr, nullptr, SOCK_NONBLOCK)};
	return fd;
}

TCPConnection Dispatcher::accept_tcp_connection()
{
	return tcp_listener.accept_tcp_connection();
}

void Dispatcher::set_handler_factory_interface(HandlerFactoryInterface *iface_)
{
	iface = iface_;
}

void Dispatcher::cancel_connection(Handler *handler, uint32_t id)
{
	auto itr = std::find_if(connections.begin(), connections.end(), [handler, id](const std::unique_ptr<Connection> &conn) {
		return handler == conn->handler && id == conn->id;
	});

	if (itr != connections.end())
	{
		cancellations.push_back(std::move(*itr));
		auto &last_elem = connections.back();
		if (&last_elem != &*itr)
			last_elem.swap(*itr);
		connections.pop_back();
	}
}

bool Dispatcher::add_connection(FileHandle fd, Handler *handler, uint32_t id, ConnectionType type)
{
	auto c = std::make_unique<Connection>();
	c->fd = std::move(fd);
	c->id = id;
	c->handler = handler;

	struct epoll_event ev = {};
	ev.data.ptr = c.get();

	if (type != ConnectionType::Output)
		ev.events |= EPOLLIN;
	if (type != ConnectionType::Input)
		ev.events |= EPOLLOUT;

	if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD,
	              c->fd.get_native_handle(), &ev) == 0)
	{
		connections.push_back(std::move(c));
		return true;
	}
	else
		return false;
}

bool Dispatcher::iterate()
{
	bool ret = iterate_inner();

	if (!ret)
	{
		// Drop all connections immediately.
		pollfd = {};
		connections.clear();
		cancellations.clear();
	}

	return ret;
}

int Dispatcher::write_udp_datagram(const RemoteAddress &addr,
                                   const void *header, unsigned header_size,
                                   const void *data, unsigned size)
{
	struct msghdr msg = {};
	msg.msg_name = const_cast<sockaddr_storage *>(&addr.addr);
	msg.msg_namelen = addr.addr_size;

	struct iovec iv[2] = {};
	msg.msg_iov = iv;
	msg.msg_iovlen = 2;

	iv[0].iov_base = const_cast<void *>(header);
	iv[0].iov_len = header_size;
	iv[1].iov_base = const_cast<void *>(data);
	iv[1].iov_len = size;

	return int(::sendmsg(udp_listener.get_file_handle().get_native_handle(), &msg, 0));
}

bool Dispatcher::iterate_inner()
{
	if (!pollfd)
		return false;

	struct epoll_event events[64] = {};
	int count = epoll_wait(pollfd.get_native_handle(), events, 64, -1);
	if (count < 0)
		return errno == EINTR;

	for (int i = 0; i < count; i++)
	{
		auto &e = events[i];
		if (e.data.ptr == &udp_listener)
		{
			// 64k is UDP limit.
			uint8_t buffer[64 * 1024];
			RemoteAddress remote;

			int ret = udp_listener.read_udp_datagram(remote, buffer, sizeof(buffer));
			if (ret > 0 && iface)
				iface->handle_udp_datagram(*this, remote, buffer, ret);
		}
		else if (e.data.ptr == &listener || e.data.ptr == &tcp_listener)
		{
			TCPConnection tcp;
			FileHandle domain;

			if (e.data.ptr == &tcp_listener)
				tcp = accept_tcp_connection();
			else
				domain = accept_connection();

			auto c = std::make_unique<Connection>();

			if (tcp.fd)
			{
				c->fd = std::move(tcp.fd);
				c->remote = tcp.addr;
			}
			else if (domain)
			{
				c->fd = std::move(domain);
			}

			if (c->fd)
			{
				struct epoll_event ev = {};
				ev.data.ptr = c.get();
				ev.events = EPOLLIN;
				if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD,
				              c->fd.get_native_handle(), &ev) == 0)
				{
					connections.push_back(std::move(c));
				}
			}
		}
		else
		{
			auto *conn = static_cast<Connection *>(e.data.ptr);
			bool hangup = false;

			if ((e.events & EPOLLHUP) != 0)
			{
				hangup = true;
			}
			else if (!conn->handler)
			{
				bool ret;
				if (conn->remote)
					ret = iface && iface->register_tcp_handler(*this, conn->fd, conn->remote, conn->handler);
				else
					ret = iface && iface->register_handler(*this, conn->fd, conn->handler);

				if (!ret)
					hangup = true;
			}
			else if (!conn->handler->handle(conn->fd, conn->id))
			{
				hangup = true;
			}

			if (hangup)
			{
				if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_DEL,
				              conn->fd.get_native_handle(), nullptr) < 0)
				{
					return false;
				}

				bool is_sentinel = conn->handler && conn->handler->is_sentinel_file_handle();

				auto itr = std::find_if(connections.begin(), connections.end(),
				                        [conn](const std::unique_ptr<Connection> &c)
				                        { return c.get() == conn; });
				assert(itr != connections.end());
				connections.erase(itr);

				if (is_sentinel)
					return false;
			}
		}
	}

	// Clean up any cancellations.
	bool is_sentinel = false;
	for (auto &cancel : cancellations)
	{
		if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_DEL,
		              cancel->fd.get_native_handle(), nullptr) < 0)
		{
			return false;
		}

		if (cancel->handler && cancel->handler->is_sentinel_file_handle())
			is_sentinel = true;

		cancel.reset();
	}

	cancellations.clear();

	return !is_sentinel;
}

struct SignalHandler final : Handler
{
	explicit SignalHandler(Dispatcher &dispatcher_)
		: Handler(dispatcher_)
	{
		set_sentinel_file_handle();
	}

	bool handle(const FileHandle &, uint32_t) override
	{
		// Instantly terminate.
		return false;
	}

	void release_id(uint32_t) override
	{
		delete this;
	}
};

void Dispatcher::block_signals()
{
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigmask, nullptr);
	signal(SIGPIPE, SIG_IGN);
}

void Dispatcher::add_signalfd()
{
	sigset_t sigmask;
	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	auto signal_handler = FileHandle(signalfd(-1, &sigmask, 0));
	if (!signal_handler)
		throw std::runtime_error("Failed to create signalfd.");

	auto conn = std::make_unique<Connection>();
	conn->fd = std::move(signal_handler);
	conn->handler = new SignalHandler{*this};

	struct epoll_event e = {};
	e.data.ptr = conn.get();
	e.events = EPOLLIN;
	if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD, conn->fd.get_native_handle(), &e) < 0)
		throw std::runtime_error("Failed to add to epoll.");
	connections.push_back(std::move(conn));
}

void Dispatcher::add_eventfd()
{
	auto efd = FileHandle(eventfd(0, 0));
	if (!efd)
		throw std::runtime_error("Failed to create efd.");

	auto conn = std::make_unique<Connection>();
	conn->fd = std::move(efd);
	conn->handler = new SignalHandler{*this};
	event_handle = &conn->fd;

	struct epoll_event e = {};
	e.data.ptr = conn.get();
	e.events = EPOLLIN;
	if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD, conn->fd.get_native_handle(), &e) < 0)
		throw std::runtime_error("Failed to add to epoll.");
	connections.push_back(std::move(conn));
}

void Dispatcher::kill()
{
	uint64_t value = 1;
	if (event_handle)
		(void)::write(event_handle->get_native_handle(), &value, sizeof(value));
}

Dispatcher::Dispatcher(const char *name, const char *listen_port)
	: listener(name)
{
	if (::listen(listener.get_file_handle().get_native_handle(), 16) < 0)
		throw std::runtime_error("Failed to listen.");

	pollfd = FileHandle(epoll_create1(EPOLL_CLOEXEC));
	if (!pollfd)
		throw std::runtime_error("Failed to create epoll FD.");

	add_signalfd();
	add_eventfd();

	struct epoll_event e = {};
	e.data.ptr = &listener;
	e.events = EPOLLIN;
	if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD, listener.get_file_handle().get_native_handle(), &e) < 0)
		throw std::runtime_error("Failed to add to epoll.");

	if (listen_port)
	{
		tcp_listener = IPListener{IPListener::Proto::TCP, listen_port};
		udp_listener = IPListener{IPListener::Proto::UDP, listen_port};
	}

	if (tcp_listener.get_file_handle() && udp_listener.get_file_handle())
	{
		if (::listen(tcp_listener.get_file_handle().get_native_handle(), 4) < 0)
			throw std::runtime_error("Failed to listen.");

		e.data.ptr = &tcp_listener;
		e.events = EPOLLIN;
		if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD, tcp_listener.get_file_handle().get_native_handle(), &e) < 0)
			throw std::runtime_error("Failed to add to epoll.");

		e.data.ptr = &udp_listener;
		e.events = EPOLLIN;
		if (epoll_ctl(pollfd.get_native_handle(), EPOLL_CTL_ADD, udp_listener.get_file_handle().get_native_handle(), &e) < 0)
			throw std::runtime_error("Failed to add to epoll.");
	}
	else if (listen_port)
		throw std::runtime_error("Failed to set up TCP and UDP listeners.");
}

RemoteAddress::operator bool() const
{
	return addr_size != 0;
}

bool RemoteAddress::operator==(const PyroFling::RemoteAddress &other) const
{
	return addr_size == other.addr_size && memcmp(&addr, &other.addr, addr_size) == 0;
}

bool RemoteAddress::operator!=(const PyroFling::RemoteAddress &other) const
{
	return !(*this == other);
}
}
