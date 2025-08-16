#include "simple_socket.hpp"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#include <winsock2.h>
#undef SHUT_RD
#define SHUT_RD SD_RECEIVE
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#define closesocket(x) ::close(x)
#endif

namespace PyroFling
{
Socket::~Socket()
{
	if (thr.joinable())
	{
		// Unblock the thread in case it's waiting for us to read data.
		{
			std::lock_guard<std::mutex> holder{lock};
			ring.read_count = ring.write_count;
			cond.notify_one();
		}

#ifdef _WIN32
		// Dirty hack since shutdown doesn't work and cba to use more complicated APIs.
		if (fd >= 0)
		{
			closesocket(fd);
			fd = -1;
		}
#else
		// If thread is blocking on a read, it should unblock now.
		if (fd >= 0)
			shutdown(fd, SHUT_RD);
#endif

		thr.join();
	}

	if (fd >= 0)
		closesocket(fd);
}

bool Socket::connect(Proto proto, const char *addr, const char *port)
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		fprintf(stderr, "Failed to initialize WSA.\n");
		return false;
	}
#endif

	addrinfo hints = {};
	addrinfo *servinfo;

	hints.ai_family = AF_UNSPEC;

	if (proto == Proto::TCP)
	{
		hints.ai_socktype = SOCK_STREAM;
#ifdef __ANDROID__
		hints.ai_protocol = 0;
#else
		hints.ai_protocol = IPPROTO_TCP;
#endif
	}
	else
	{
		hints.ai_socktype = SOCK_DGRAM;
#ifdef __ANDROID__
		hints.ai_protocol = 0;
#else
		hints.ai_protocol = IPPROTO_UDP;
#endif
	}

	int res = getaddrinfo(addr, port, &hints, &servinfo);
	if (res < 0)
		return false;

	addrinfo *walk;
	for (walk = servinfo; walk; walk = walk->ai_next)
	{
		int new_fd = socket(walk->ai_family, walk->ai_socktype, walk->ai_protocol);
		if (new_fd < 0)
			return false;
		if (::connect(new_fd, walk->ai_addr, walk->ai_addrlen) < 0)
		{
			closesocket(new_fd);
			continue;
		}
		fd = new_fd;
		break;
	}

	freeaddrinfo(servinfo);

	if (proto == Proto::UDP)
	{
		// Keep the rcvbuf healthy so we don't drop packets too easily.
		int size = 4 * 1024 * 1024;
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&size), sizeof(size)) < 0)
			return false;

		int actual_size = 0;
		socklen_t sizelen = sizeof(size);
		getsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&actual_size), &sizelen);
		fprintf(stderr, "Actual UDP rcvbuf size: %d bytes\n", actual_size);
	}

	if (!walk)
		return false;

	return true;
}

bool Socket::init_recv_thread(size_t max_packet_size, size_t num_packets)
{
	if (thr.joinable())
		return false;

	if (num_packets & (num_packets - 1))
	{
		fprintf(stderr, "num_packets must be POT.\n");
		return false;
	}

	ring.packets.clear();
	ring.packets.reserve(num_packets);
	for (size_t i = 0; i < num_packets; i++)
		ring.packets.push_back({ std::unique_ptr<char []>{new char[max_packet_size]}, 0 });
	ring.max_packet_size = max_packet_size;

	try
	{
		thr = std::thread(&Socket::recv_thread, this);
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "Failed to create thread.\n");
		return false;
	}

	return true;
}

void Socket::recv_thread()
{
	uint32_t mask = ring.packets.size() - 1;

	for (;;)
	{
		{
			std::unique_lock<std::mutex> holder{lock};
			cond.wait(holder, [this]() {
				uint32_t queued = ring.write_count - ring.read_count;
				return queued < ring.packets.size();
			});
		}

		auto &packet = ring.packets[ring.write_count & mask];

		int ret = int(::recv(fd, packet.data.get(), ring.max_packet_size, 0));
		if (ret <= 0)
			break;

		packet.size = ret;
		std::lock_guard<std::mutex> holder{lock};
		ring.write_count++;
		cond.notify_one();
	}

	std::lock_guard<std::mutex> holder{lock};
	ring.dead = true;
	cond.notify_one();
}

size_t Socket::read_thread_packet(void *data, size_t size)
{
	bool has_packet = false;

	{
		std::unique_lock<std::mutex> holder{lock};
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		if (!cond.wait_until(holder, deadline, [this]() {
			return ring.dead || ring.write_count != ring.read_count; }))
		{
			return 0;
		}

		has_packet = ring.write_count != ring.read_count;
	}

	if (!has_packet)
		return 0;

	auto &packet = ring.packets[ring.read_count & (ring.packets.size() - 1)];

	size = std::min<size_t>(size, packet.size);
	memcpy(data, packet.data.get(), size);

	std::lock_guard<std::mutex> holder{lock};
	ring.read_count++;
	cond.notify_one();
	return size;
}

bool Socket::read(void *data_, size_t size, const Socket *sentinel)
{
	auto *data = static_cast<uint8_t *>(data_);

	while (size)
	{
		// Unified to be compat with Windows as well.
		timeval tv = {};
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		int nfds = fd + 1;
		if (sentinel)
		{
			FD_SET(sentinel->fd, &fds);
			if (sentinel->fd > fd)
				nfds = sentinel->fd + 1;
		}

		tv.tv_sec = 5;
		if (select(nfds, &fds, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd, &fds))
		{
			int ret;
			if ((ret = int(::recv(fd, reinterpret_cast<char *>(data), size, 0))) <= 0)
				return false;
			size -= ret;
			data += ret;
		}
		else
			return false;
	}

	return true;
}

size_t Socket::read_partial(void *data, size_t size, const Socket *sentinel)
{
	// Unified to be compat with Windows as well.
	timeval tv = {};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	int nfds = fd + 1;
	if (sentinel)
	{
		FD_SET(sentinel->fd, &fds);
		if (sentinel->fd > fd)
			nfds = sentinel->fd + 1;
	}

	tv.tv_sec = 5;
	if (select(nfds, &fds, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd, &fds))
	{
		int ret;
		if ((ret = int(::recv(fd, reinterpret_cast<char *>(data), size, 0))) <= 0)
			return false;
		else
			return size_t(ret);
	}
	else
		return false;
}

#ifdef __linux__
static constexpr int MSG_FLAG = MSG_NOSIGNAL;
#else
static constexpr int MSG_FLAG = 0;
#endif

bool Socket::write(const void *data_, size_t size)
{
	auto *data = static_cast<const uint8_t *>(data_);

	while (size)
	{
		int ret;
		if ((ret = int(::send(fd, reinterpret_cast<const char *>(data), size, MSG_FLAG))) <= 0)
			return false;
		size -= ret;
		data += ret;
	}

	return true;
}

bool Socket::write_message(const void *header, size_t header_size, const void *data, size_t size)
{
#ifdef _WIN32
	uint8_t buffer[64 * 1024];
	if (header_size + size > sizeof(buffer))
		return false;

	memcpy(buffer, header, header_size);
	memcpy(buffer + header_size, data, size);
	return write(buffer, header_size + size);
#else
	struct msghdr msg = {};
	struct iovec iv[2] = {};

	iv[0].iov_base = const_cast<void *>(header);
	iv[0].iov_len = header_size;
	iv[1].iov_base = const_cast<void *>(data);
	iv[1].iov_len = size;

	msg.msg_iovlen = 2;
	msg.msg_iov = iv;

	return ::sendmsg(fd, &msg, MSG_FLAG) == ssize_t(header_size + size);
#endif
}
}
