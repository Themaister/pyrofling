#include "simple_socket.hpp"

#ifdef _WIN32
#include <ws2tcpip.h>
#include <winsock2.h>
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
	if (fd >= 0)
		closesocket(fd);
}

bool Socket::connect(Proto proto, const char *addr, const char *port)
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		LOGE("Failed to initialize WSA.\n");
		return false;
	}
#endif

	addrinfo hints = {};
	addrinfo *servinfo;

	hints.ai_family = AF_UNSPEC;

	if (proto == Proto::TCP)
	{
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
	}
	else
	{
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
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

	if (!walk)
		return false;

	return true;
}

bool Socket::read(void *data_, size_t size)
{
	auto *data = static_cast<uint8_t *>(data_);

	while (size)
	{
		// Unified to be compat with Windows as well.
		timeval tv = {};
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		tv.tv_sec = 5;
		if (select(fd + 1, &fds, nullptr, nullptr, &tv) > 0)
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

size_t Socket::read_partial(void *data, size_t size)
{
	// Unified to be compat with Windows as well.
	timeval tv = {};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	tv.tv_sec = 5;
	if (select(fd + 1, &fds, nullptr, nullptr, &tv) > 0)
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

bool Socket::write(const void *data_, size_t size)
{
	auto *data = static_cast<const uint8_t *>(data_);

	while (size)
	{
		int ret;
		if ((ret = int(::send(fd, reinterpret_cast<const char *>(data), size, 0))) <= 0)
			return false;
		size -= ret;
		data += ret;
	}

	return true;
}
}
