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
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		for (unsigned i = 0; i < 4096; i++)
		{
			uint8_t buf[12000];
			for (unsigned j = 0; j < sizeof(buf); j++)
				buf[j] = uint8_t(i + j * 17);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			server.write_video_packet(i, i, buf, sizeof(buf), i % 16 == 0);
			//server.write_audio_packet(i, i, buf, sizeof(buf));
		}
		dispatcher.kill();
	});

	PyroStreamClient client;
	if (!client.connect("127.0.0.1", "8080"))
		return EXIT_FAILURE;
	if (!client.handshake(PYRO_KICK_STATE_VIDEO_BIT | PYRO_KICK_STATE_AUDIO_BIT))
		return EXIT_FAILURE;

	PyroStreamClient::set_simulate_drop(false);
	PyroStreamClient::set_simulate_reordering(true);

	while (client.wait_next_packet())
	{
		auto &header = client.get_payload_header();
		const auto *data = static_cast<const uint8_t *>(client.get_packet_data());
		size_t size = client.get_packet_size();

		unsigned long long pts = header.pts_lo | (uint64_t(header.pts_hi) << 32);
		unsigned long long dts = pts - header.dts_delta;
		bool is_audio = (header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) != 0;
		uint32_t seq = pyro_payload_get_packet_seq(header.encoded);

		printf("%s (%zu) || pts = %llu, dts = %llu, seq = %u, key = %u\n", is_audio ? "audio" : "video",
			   size, pts, dts, seq, (header.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT) != 0 ? 1 : 0);
		bool valid = true;
		for (size_t i = 0; i < size && valid; i++)
		{
			auto expected = uint8_t(pts + 17 * i);
			if (data[i] != expected)
				valid = false;
		}
		printf("   Valid: %u\n", valid);
	}

	sender.join();
	thr.join();
}
