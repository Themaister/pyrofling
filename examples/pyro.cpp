#include "listener.hpp"
#include "pyro_server.hpp"
#include "pyro_client.hpp"
#include "pyro_protocol.h"

#include <thread>
#include <chrono>

struct Server final : PyroFling::HandlerFactoryInterface
{
	void handle_udp_datagram(PyroFling::Dispatcher &dispatcher, const PyroFling::RemoteAddress &remote,
	                         const void *msg, unsigned size) override
	{
		pyro.handle_udp_datagram(dispatcher, remote, msg, size);
	}

	bool register_handler(PyroFling::Dispatcher &, const PyroFling::FileHandle &,
	                      PyroFling::Handler *&) override
	{
		return false;
	}

	bool register_tcp_handler(PyroFling::Dispatcher &dispatcher, const PyroFling::FileHandle &fd,
	                          const PyroFling::RemoteAddress &remote, PyroFling::Handler *&handler) override
	{
		return pyro.register_tcp_handler(dispatcher, fd, remote, handler);
	}

	void set_codec_parameters(const pyro_codec_parameters &param)
	{
		pyro.set_codec_parameters(param);
	}

	void write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame)
	{
		pyro.write_video_packet(pts, dts, data, size, is_key_frame);
	}

	void write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size)
	{
		pyro.write_audio_packet(pts, dts, data, size);
	}

	PyroFling::PyroStreamServer pyro;
};

int main()
{
	using namespace PyroFling;
	Dispatcher::block_signals();
	Server server;
	Dispatcher dispatcher("/tmp/pyro", "8080");

	dispatcher.set_handler_factory_interface(&server);
	std::thread thr([&dispatcher]() { while (dispatcher.iterate()); });
	std::thread sender([&server, &dispatcher]() {
		pyro_codec_parameters params = {};
		params.video_codec = PYRO_VIDEO_CODEC_H264;
		server.set_codec_parameters(params);
		for (unsigned i = 0; i < 64; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			server.write_video_packet(i + 100, i, &i, sizeof(i), i % 16 == 0);
			server.write_audio_packet(i + 1000, i + 1000, &i, sizeof(i));
		}
		dispatcher.kill();
	});

	PyroStreamClient client;
	if (!client.connect("127.0.0.1", "8080"))
		return EXIT_FAILURE;
	if (!client.handshake())
		return EXIT_FAILURE;

	while (client.wait_next_packet())
	{
		auto &header = client.get_payload_header();
		const auto *data = static_cast<const uint8_t *>(client.get_packet_data());
		size_t size = client.get_packet_size();

		unsigned long long pts = header.pts_lo | (uint64_t(header.pts_hi) << 32);
		unsigned long long dts = pts - header.dts_delta;
		bool is_audio = (header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) != 0;
		uint32_t seq = pyro_payload_get_packet_seq(header.encoded);

		printf("%s || pts = %llu, dts = %llu, seq = %u, key = %u\n", is_audio ? "audio" : "video",
		       pts, dts, seq, (header.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT) != 0 ? 1 : 0);
		printf("  ");
		for (size_t i = 0; i < size; i++)
			printf("%02x", data[i]);
		printf("\n");
	}

	sender.join();
	thr.join();
}
