#pragma once
#include <stddef.h>

namespace PyroFling
{
class Socket
{
public:
	Socket() = default;
	~Socket();
	void operator=(const Socket &) = delete;
	Socket(const Socket &) = delete;

	enum class Proto { TCP, UDP };
	bool connect(Proto proto, const char *addr, const char *port);
	size_t read_partial(void *data, size_t size, const Socket *sentinel = nullptr);
	bool read(void *data, size_t size, const Socket *sentinel = nullptr);
	bool write(const void *data, size_t size);
	bool write_message(const void *header, size_t header_size, const void *data, size_t size);

private:
	int fd = -1;
};
}