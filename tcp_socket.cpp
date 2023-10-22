#include "tcp_socket.hpp"

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
TCPReader::~TCPReader()
{
	if (fd >= 0)
		closesocket(fd);
}

bool TCPReader::connect(const char *addr, const char *port)
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
	hints.ai_socktype = SOCK_STREAM;

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

bool TCPReader::read(void *data_, size_t size)
{
	auto *data = static_cast<uint8_t *>(data_);

	while (size)
	{
		int ret;
		if ((ret = ::recv(fd, reinterpret_cast<char *>(data), size, 0)) <= 0)
			return false;
		size -= ret;
		data += ret;
	}

	return true;
}
}
