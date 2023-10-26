#include "simple_socket.hpp"
#include "listener.hpp"
#include "messages.hpp"

#include <vector>
#include <thread>
#include <algorithm>
#include <mutex>
#include <chrono>

#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <netdb.h>

#include "pyro_protocol.h"
#include "intrusive.hpp"

class PyroStreamConnection;

class PyroStreamConnectionServerInterface
{
public:
	virtual ~PyroStreamConnectionServerInterface() = default;
	virtual void release_connection(PyroStreamConnection *conn) = 0;
	virtual pyro_codec_parameters get_codec_parameters() = 0;
};

class PyroStreamConnection : public PyroFling::Handler, public Util::IntrusivePtrEnabled<PyroStreamConnection>
{
public:
	PyroStreamConnection(PyroFling::Dispatcher &dispatcher, PyroStreamConnectionServerInterface &server,
	                     const PyroFling::RemoteAddress &tcp_remote, uint64_t cookie);

	bool handle(const PyroFling::FileHandle &fd, uint32_t id) override;
	void release_id(uint32_t id) override;

	void write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame);
	void write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size);

	void handle_udp_datagram(PyroFling::Dispatcher &dispatcher,
	                         const PyroFling::RemoteAddress &remote,
	                         const void *msg, size_t size);

private:
	PyroStreamConnectionServerInterface &server;
	PyroFling::RemoteAddress tcp_remote;
	PyroFling::RemoteAddress udp_remote;
	PyroFling::FileHandle timer_fd;
	pyro_progress_report progress = {};

	uint64_t cookie;
	uint32_t packet_seq_video = 0;
	uint32_t packet_seq_audio = 0;

	union
	{
		unsigned char buffer[PYRO_MAX_MESSAGE_BUFFER_LENGTH];
		struct
		{
			pyro_message_type type;
			unsigned char payload[PYRO_MAX_MESSAGE_BUFFER_LENGTH - sizeof(pyro_message_type)];
		} split;
	} tcp;
	uint32_t tcp_length = 0;

	bool kicked = false;
	void write_packet(int64_t pts, int64_t dts, const void *data_, size_t size, bool is_audio, bool is_key_frame);
};

struct Server final : PyroFling::HandlerFactoryInterface, PyroStreamConnectionServerInterface
{
	bool register_handler(PyroFling::Dispatcher &, const PyroFling::FileHandle &, PyroFling::Handler *&) override
	{
		return false;
	}

	void set_codec_parameters(const pyro_codec_parameters &codec_)
	{
		std::lock_guard<std::mutex> holder{lock};
		codec = codec_;
	}

	pyro_codec_parameters get_codec_parameters() override
	{
		std::lock_guard<std::mutex> holder{lock};
		return codec;
	}

	bool register_tcp_handler(PyroFling::Dispatcher &dispatcher, const PyroFling::FileHandle &,
	                          const PyroFling::RemoteAddress &remote,
	                          PyroFling::Handler *&handler) override
	{
		auto conn = Util::make_handle<PyroStreamConnection>(dispatcher, *this, remote, ++cookie);
		conn->add_reference();
		handler = conn.get();
		std::lock_guard<std::mutex> holder{lock};
		connections.push_back(std::move(conn));
		return true;
	}

	void write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame)
	{
		std::lock_guard<std::mutex> holder{lock};
		for (auto &conn : connections)
			conn->write_video_packet(pts, dts, data, size, is_key_frame);
	}

	void write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size)
	{
		std::lock_guard<std::mutex> holder{lock};
		for (auto &conn : connections)
			conn->write_audio_packet(pts, dts, data, size);
	}

	void handle_udp_datagram(PyroFling::Dispatcher &dispatcher, const PyroFling::RemoteAddress &remote,
	                         const void *msg, unsigned size) override
	{
		for (auto &conn : connections)
			conn->handle_udp_datagram(dispatcher, remote, msg, size);
	}

	void release_connection(PyroStreamConnection *conn) override
	{
		std::lock_guard<std::mutex> holder{lock};
		auto itr = std::find_if(connections.begin(), connections.end(),
		                        [conn](const Util::IntrusivePtr<PyroStreamConnection> &ptr) {
			                        return ptr.get() == conn;
		                        });
		if (itr != connections.end())
			connections.erase(itr);
	}

	uint64_t cookie = 1000;
	std::mutex lock;
	std::vector<Util::IntrusivePtr<PyroStreamConnection>> connections;
	pyro_codec_parameters codec;
};

PyroStreamConnection::PyroStreamConnection(
		PyroFling::Dispatcher &dispatcher_, PyroStreamConnectionServerInterface &server_,
		const PyroFling::RemoteAddress &remote, uint64_t cookie_)
	: PyroFling::Handler(dispatcher_)
	, server(server_)
	, tcp_remote(remote)
	, cookie(cookie_)
{
	packet_seq_video = cookie & ((1 << PYRO_PAYLOAD_PACKET_SEQ_BITS) - 1);
	packet_seq_audio = (~cookie) & ((1 << PYRO_PAYLOAD_PACKET_SEQ_BITS) - 1);

	timer_fd = PyroFling::FileHandle(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC));
	add_reference();
	dispatcher.add_connection(timer_fd.dup(), this, 1, PyroFling::Dispatcher::ConnectionType::Input);
}

bool PyroStreamConnection::handle(const PyroFling::FileHandle &fd, uint32_t id)
{
	// Timeout, cancel everything.
	if (id)
	{
		dispatcher.cancel_connection(this, 0);
		return false;
	}

	// We've exhausted the buffer.
	if (tcp_length >= sizeof(tcp.buffer))
		return false;

	size_t ret = receive_stream_message(fd, tcp.buffer + tcp_length, sizeof(tcp.buffer) - tcp_length);
	if (!ret)
		return false;

	tcp_length += ret;

	while (tcp_length >= sizeof(pyro_message_type) &&
	       tcp_length >= pyro_message_get_length(tcp.split.type) + sizeof(pyro_message_type))
	{
		if (!pyro_message_validate_magic(tcp.split.type))
			return false;

		switch (tcp.split.type)
		{
		case PYRO_MESSAGE_HELLO:
		{
			const pyro_message_type type = PYRO_MESSAGE_COOKIE;
			if (!send_stream_message(fd, &type, sizeof(type)))
				return false;
			if (!send_stream_message(fd, &cookie, sizeof(cookie)))
				return false;
			break;
		}

		case PYRO_MESSAGE_KICK:
		{
			if (kicked)
				return false;

			auto codec = server.get_codec_parameters();

			if (udp_remote && codec.video_codec != PYRO_VIDEO_CODEC_NONE)
			{
				const pyro_message_type type = PYRO_MESSAGE_CODEC_PARAMETERS;
				if (!send_stream_message(fd, &type, sizeof(type)))
					return false;
				if (!send_stream_message(fd, &codec, sizeof(codec)))
					return false;
			}
			else if (udp_remote)
			{
				const pyro_message_type type = PYRO_MESSAGE_AGAIN;
				if (!send_stream_message(fd, &type, sizeof(type)))
					return false;
			}
			else
			{
				const pyro_message_type type = PYRO_MESSAGE_NAK;
				if (!send_stream_message(fd, &type, sizeof(type)))
					return false;
			}

			kicked = true;

			// Start timeout.
			struct itimerspec tv = {};
			tv.it_value.tv_sec = 5;
			timerfd_settime(timer_fd.get_native_handle(), 0, &tv, nullptr);

			break;
		}

		case PYRO_MESSAGE_PROGRESS:
		{
			// Re-start timeout.
			struct itimerspec tv = {};
			tv.it_value.tv_sec = 5;
			timerfd_settime(timer_fd.get_native_handle(), 0, &tv, nullptr);
			memcpy(&progress, tcp.split.payload, sizeof(progress));
			break;
		}

		default:
			// Invalid message.
			return false;
		}

		size_t move_offset = pyro_message_get_length(tcp.split.type) + sizeof(pyro_message_type);
		memmove(tcp.buffer, tcp.buffer + move_offset, tcp_length - move_offset);
		tcp_length -= move_offset;
	}

	return true;
}

void PyroStreamConnection::release_id(uint32_t id)
{
	if (id == 0)
		server.release_connection(this);
	release_reference();
}

void PyroStreamConnection::write_packet(int64_t pts, int64_t dts,
                                        const void *data_, size_t size,
                                        bool is_audio, bool is_key_frame)
{
	if (!udp_remote || !kicked)
		return;

	auto &seq = is_audio ? packet_seq_audio : packet_seq_video;

	pyro_payload_header header = {};
	header.pts_lo = uint32_t(pts);
	header.pts_hi = uint32_t(pts >> 32);
	header.dts_delta = uint32_t(pts - dts);
	header.encoded |= is_audio ? PYRO_PAYLOAD_STREAM_TYPE_BIT : 0;
	header.encoded |= is_key_frame ? PYRO_PAYLOAD_KEY_FRAME_BIT : 0;
	header.encoded |= seq << PYRO_PAYLOAD_PACKET_SEQ_OFFSET;
	uint32_t subseq = 0;

	auto *data = static_cast<const uint8_t *>(data_);
	for (size_t i = 0; i < size; i += PYRO_MAX_PAYLOAD_SIZE, data += PYRO_MAX_PAYLOAD_SIZE)
	{
		header.encoded &= ~(PYRO_PAYLOAD_PACKET_BEGIN_BIT | PYRO_PAYLOAD_PACKET_DONE_BIT);
		if (i == 0)
			header.encoded |= PYRO_PAYLOAD_PACKET_BEGIN_BIT;
		if (i + PYRO_MAX_PAYLOAD_SIZE >= size)
			header.encoded |= PYRO_PAYLOAD_PACKET_DONE_BIT;

		header.encoded &= ~(PYRO_PAYLOAD_SUBPACKET_SEQ_MASK << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET);
		header.encoded |= subseq << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET;

		dispatcher.write_udp_datagram(udp_remote, &header, sizeof(header),
		                              data, std::min<size_t>(PYRO_MAX_PAYLOAD_SIZE, size - i));

		subseq = (subseq + 1) & PYRO_PAYLOAD_SUBPACKET_SEQ_MASK;
	}

	seq = (seq + 1) & PYRO_PAYLOAD_PACKET_SEQ_MASK;
}

void PyroStreamConnection::write_video_packet(int64_t pts, int64_t dts,
                                              const void *data, size_t size, bool is_key_frame)
{
	write_packet(pts, dts, data, size, false, is_key_frame);
}

void PyroStreamConnection::write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size)
{
	write_packet(pts, dts, data, size, true, false);
}

void PyroStreamConnection::handle_udp_datagram(
		PyroFling::Dispatcher &, const PyroFling::RemoteAddress &remote,
		const void *msg_, size_t size)
{
	auto *msg = static_cast<const uint8_t *>(msg_);
	if (size < sizeof(pyro_message_type))
		return;

	pyro_message_type type;
	memcpy(&type, msg, sizeof(type));

	if (!pyro_message_validate_magic(type))
		return;
	if (pyro_message_get_length(type) + sizeof(type) != size)
		return;

	msg += sizeof(type);

	switch (type)
	{
	case PYRO_MESSAGE_COOKIE:
	{
		if (memcmp(msg, &cookie, sizeof(cookie)) == 0 && !udp_remote)
			udp_remote = remote;
		break;
	}

	default:
		break;
	}
}

class PyroStreamClient
{
public:
	bool connect(const char *host, const char *port);
	bool handshake();

	const pyro_codec_parameters &get_codec_parameters() const;
	bool wait_next_packet();

	const void *get_packet_data() const;
	size_t get_packet_size() const;
	const pyro_payload_header &get_payload_header() const;

private:
	PyroFling::Socket tcp, udp;

	struct ReconstructedPacket
	{
		std::vector<uint8_t> buffer;
		std::vector<bool> subseq_flags;
		uint32_t num_done_subseqs = 0;
		bool has_done_bit = false;
		uint32_t packet_seq = 0;
		int subpacket_seq_accum = 0;
		uint32_t last_subpacket_raw_seq = 0;
		pyro_payload_header payload = {};

		void reset();
		bool is_complete() const;
	};

	uint32_t last_completed_video_seq = UINT32_MAX;
	uint32_t last_completed_audio_seq = UINT32_MAX;
	pyro_progress_report progress = {};

	ReconstructedPacket video[2];
	ReconstructedPacket audio[2];
	const ReconstructedPacket *current = nullptr;
	pyro_codec_parameters codec = {};

	std::chrono::time_point<std::chrono::steady_clock> last_progress_time;

	bool iterate();
};

bool PyroStreamClient::connect(const char *host, const char *port)
{
	if (!tcp.connect(PyroFling::Socket::Proto::TCP, host, port))
		return false;
	if (!udp.connect(PyroFling::Socket::Proto::UDP, host, port))
		return false;

	return true;
}

bool PyroStreamClient::handshake()
{
	pyro_message_type type = PYRO_MESSAGE_HELLO;
	if (!tcp.write(&type, sizeof(type)))
		return false;

	if (!tcp.read(&type, sizeof(type)) || type != PYRO_MESSAGE_COOKIE)
		return false;

	uint64_t cookie = 0;
	if (!tcp.read(&cookie, sizeof(cookie)))
		return false;

	for (unsigned i = 0; i < 16 && codec.video_codec == PYRO_VIDEO_CODEC_NONE; i++)
	{
		type = PYRO_MESSAGE_COOKIE;
		if (!udp.write_message(&type, sizeof(type), &cookie, sizeof(cookie)))
			return false;

		type = PYRO_MESSAGE_KICK;
		if (!tcp.write(&type, sizeof(type)))
			return false;

		if (!tcp.read(&type, sizeof(type)))
			return false;
		if (type != PYRO_MESSAGE_CODEC_PARAMETERS)
			continue;
		if (!tcp.read(&codec, sizeof(codec)))
			return false;
	}

	last_progress_time = std::chrono::steady_clock::now();
	return codec.video_codec != PYRO_VIDEO_CODEC_NONE;
}

const void *PyroStreamClient::get_packet_data() const
{
	return current ? current->buffer.data() : nullptr;
}

size_t PyroStreamClient::get_packet_size() const
{
	return current ? current->buffer.size() : 0;
}

const pyro_codec_parameters &PyroStreamClient::get_codec_parameters() const
{
	return codec;
}

void PyroStreamClient::ReconstructedPacket::reset()
{
	buffer.clear();
	subseq_flags.clear();
	last_subpacket_raw_seq = 0;
	subpacket_seq_accum = 0;
	num_done_subseqs = 0;
	has_done_bit = false;
	packet_seq = 0;
}

bool PyroStreamClient::ReconstructedPacket::is_complete() const
{
	return num_done_subseqs == subseq_flags.size() && has_done_bit;
}

const pyro_payload_header &PyroStreamClient::get_payload_header() const
{
	return current->payload;
}

bool PyroStreamClient::iterate()
{
	struct
	{
		pyro_payload_header header;
		uint8_t buffer[PYRO_MAX_PAYLOAD_SIZE];
	} payload;

	size_t size = udp.read_partial(&payload, sizeof(payload), &tcp);

	if (size <= sizeof(pyro_payload_header))
		return false;
	size -= sizeof(pyro_payload_header);

	// Partial packets must be full packets (for simplicity to avoid stitching payloads together).
	if ((payload.header.encoded & PYRO_PAYLOAD_PACKET_DONE_BIT) == 0 && size != PYRO_MAX_PAYLOAD_SIZE)
		return false;

	bool is_audio = (payload.header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) != 0;

	auto *stream_base = is_audio ? &audio[0] : &video[0];
	auto &last_completed_seq = is_audio ? last_completed_audio_seq : last_completed_video_seq;
	auto &h = payload.header;

	uint32_t packet_seq = pyro_payload_get_packet_seq(h.encoded);

	// Either we work on an existing packet,
	// drop the packet if it's too old, or discard existing packets if we start receiving subpackets that obsolete
	// the existing packet.

	// Principle of the implementation is to commit to a packet when it has been completed.
	// Only allow one packet to be received out of order.
	// Only retire packets monotonically.

	// Duplicate packets most likely, or very old packets were sent.
	if (last_completed_seq != UINT32_MAX && pyro_payload_get_packet_seq_delta(packet_seq, last_completed_seq) <= 0)
		return true;

	ReconstructedPacket *stream;
	if (packet_seq == stream_base[0].packet_seq || stream_base[0].buffer.empty())
	{
		// Trivial case.
		stream = &stream_base[0];
		stream->packet_seq = packet_seq;
	}
	else if (packet_seq == stream_base[1].packet_seq && !stream_base[1].buffer.empty())
	{
		// Trivially keep appending to existing packet.
		stream = &stream_base[1];
		stream->packet_seq = packet_seq;
	}
	else if (pyro_payload_get_packet_seq_delta(packet_seq, stream_base[0].packet_seq) == 1 &&
	         stream_base[1].buffer.empty())
	{
		// We're working on stream[0], but we started to receive packets for seq + 1.
		// This is fine, just start working on stream[1].
		stream = &stream_base[1];
		stream->packet_seq = packet_seq;
	}
	else if (pyro_payload_get_packet_seq_delta(packet_seq, stream_base[0].packet_seq) == -1 &&
	         stream_base[1].buffer.empty())
	{
		// We're working on stream[0], but we got packets for stream[-1].
		// Shift the window so that they become stream 1 and 0 respectively.
		std::swap(stream_base[0], stream_base[1]);
		stream = &stream_base[0];
		stream->packet_seq = packet_seq;
	}
	else if (pyro_payload_get_packet_seq_delta(packet_seq, stream_base[0].packet_seq) < 0)
	{
		// Drop packet.
		return true;
	}
	else
	{
		// Restart case. Consider existing buffers completely stale.
		stream_base[0].reset();
		stream_base[1].reset();
		stream = &stream_base[0];
		stream->packet_seq = packet_seq;
	}

	// Copy payload data into correct offset.
	uint32_t subpacket_seq = pyro_payload_get_subpacket_seq(h.encoded);
	stream->subpacket_seq_accum += pyro_payload_get_subpacket_seq_delta(subpacket_seq, stream->last_subpacket_raw_seq);
	stream->last_subpacket_raw_seq = subpacket_seq;

	// Error case, we received bogus out-of-order sequences.
	if (stream->subpacket_seq_accum < 0)
		return true;

	// Locally, allow maximum packet size: 128 MiB.
	if (stream->subpacket_seq_accum > 128 * 1024)
		return true;

	// Error, subsequence 0 must be BEGIN flag.
	if (stream->subpacket_seq_accum == 0 && (h.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT) == 0)
		return true;

	// Error, subsequence != 0 must not be BEGIN flag.
	if (stream->subpacket_seq_accum != 0 && (h.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT) != 0)
		return true;

	if ((h.encoded & PYRO_PAYLOAD_PACKET_DONE_BIT) != 0)
	{
		stream->buffer.resize(stream->subpacket_seq_accum * PYRO_MAX_PAYLOAD_SIZE + size);
		stream->has_done_bit = true;
		stream->subseq_flags.resize(stream->subpacket_seq_accum + 1);
	}
	else
	{
		stream->buffer.resize(std::max<size_t>(
				stream->buffer.size(), (stream->subpacket_seq_accum + 1) * PYRO_MAX_PAYLOAD_SIZE));
		stream->subseq_flags.resize(std::max<size_t>(
				stream->subseq_flags.size(), (stream->subpacket_seq_accum + 1)));
	}

	if (!stream->subseq_flags[stream->subpacket_seq_accum])
	{
		stream->subseq_flags[stream->subpacket_seq_accum] = true;
		stream->num_done_subseqs++;
		memcpy(stream->buffer.data() + stream->subpacket_seq_accum * PYRO_MAX_PAYLOAD_SIZE,
			   payload.buffer, size);
	}

	if (stream->is_complete())
	{
		// We completed stream[1] before stream[0].
		// Discard stream[0] since it's out of date now.
		// We will not wait for stream[0] to eventually complete.
		if (stream == &stream_base[1])
		{
			stream_base[0].reset();
			std::swap(stream_base[0], stream_base[1]);
			stream = &stream_base[0];
		}

		if (last_completed_seq != UINT32_MAX)
		{
			int delta = pyro_payload_get_packet_seq_delta(stream->packet_seq, last_completed_seq);

			if (delta < 1)
			{
				// Bogus case. Something has gone very wrong!
				return false;
			}

			progress.total_dropped_packets += delta - 1;
		}

		last_completed_seq = stream->packet_seq;
		stream->payload = payload.header;
		progress.total_received_packets++;

		auto current_time = std::chrono::steady_clock::now();
		auto delta = current_time - last_progress_time;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() >= 1000)
		{
			last_progress_time = current_time;
			const pyro_message_type type = PYRO_MESSAGE_PROGRESS;
			if (!tcp.write(&type, sizeof(type)))
				return false;
			if (!tcp.write(&progress, sizeof(progress)))
				return false;
		}

		current = stream;
	}

	return true;
}

bool PyroStreamClient::wait_next_packet()
{
	ReconstructedPacket *clear_packet = nullptr;
	if (current == &video[0])
	{
		std::swap(video[0], video[1]);
		clear_packet = &video[1];
	}
	else if (current == &audio[0])
	{
		std::swap(audio[0], audio[1]);
		clear_packet = &audio[1];
	}

	if (clear_packet)
		clear_packet->reset();

	current = nullptr;

	while (!current)
		if (!iterate())
			return false;

	return true;
}

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
