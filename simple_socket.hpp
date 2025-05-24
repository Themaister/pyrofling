#pragma once
#include <stddef.h>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <memory>

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

	size_t read_thread_packet(void *data, size_t size);
	bool init_recv_thread(size_t max_packet_size, size_t num_packets);

private:
	int fd = -1;
	std::thread thr;
	std::condition_variable cond;
	std::mutex lock;

	struct
	{
		size_t max_packet_size = 0;
		uint32_t write_count = 0;
		uint32_t read_count = 0;
		bool dead = false;

		struct Packet
		{
			std::unique_ptr<char []> data;
			size_t size = 0;
		};
		std::vector<Packet> packets;
	} ring;

	void recv_thread();
};
}