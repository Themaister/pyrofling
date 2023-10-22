#pragma once
#include <stddef.h>
#include "ffmpeg_decode.hpp"

namespace PyroFling
{
class TCPReader : public Granite::DemuxerIOInterface
{
public:
	TCPReader() = default;
	~TCPReader();
	void operator=(const TCPReader &) = delete;
	TCPReader(const TCPReader &) = delete;

	bool connect(const char *addr, const char *port);
	bool read(void *data, size_t size) override;

private:
	int fd = -1;
};
}