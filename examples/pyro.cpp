#include "simple_socket.hpp"
#include "listener.hpp"
#include "messages.hpp"
#include <vector>
#include <thread>
#include <algorithm>
#include <mutex>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>

#include "pyro_protocol.h"

class PyroStreamConnection;

class PyroStreamConnectionCancelInterface
{
public:
	virtual ~PyroStreamConnectionCancelInterface() = default;
	virtual void release_connection(PyroStreamConnection *conn) = 0;
};

class PyroStreamConnection : public PyroFling::Handler
{
public:
	PyroStreamConnection(PyroFling::Dispatcher &dispatcher, PyroStreamConnectionCancelInterface &server,
	                     const PyroFling::RemoteAddress &tcp_remote, uint64_t cookie);
	void set_codec_parameters(const pyro_codec_parameters &parameters);

	bool handle(const PyroFling::FileHandle &fd, uint32_t) override;
	void release_id(uint32_t) override;

	void write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame);
	void write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size);

	void handle_udp_datagram(PyroFling::Dispatcher &dispatcher,
	                         const PyroFling::RemoteAddress &remote,
	                         const void *msg, size_t size);

private:
	PyroStreamConnectionCancelInterface &server;
	PyroFling::RemoteAddress tcp_remote;
	PyroFling::RemoteAddress udp_remote;

	uint64_t cookie;
	uint32_t packet_seq_video = 0;
	uint32_t packet_seq_audio = 0;

	union
	{
		pyro_message_type type;
		unsigned char buffer[PYRO_MAX_MESSAGE_BUFFER_LENGTH];
	} tcp;
	uint32_t tcp_length = 0;

	pyro_codec_parameters codec = {};
	bool kicked = false;

	void write_packet(int64_t pts, int64_t dts, const void *data_, size_t size, bool is_audio, bool is_key_frame);
};

struct Server : PyroFling::HandlerFactoryInterface, PyroStreamConnectionCancelInterface
{
	bool register_handler(PyroFling::Dispatcher &, const PyroFling::FileHandle &, PyroFling::Handler *&) override
	{
		return false;
	}

	bool register_tcp_handler(PyroFling::Dispatcher &dispatcher, const PyroFling::FileHandle &,
	                          const PyroFling::RemoteAddress &remote,
	                          PyroFling::Handler *&handler) override
	{
		auto conn = std::make_unique<PyroStreamConnection>(dispatcher, *this, remote, ++cookie);
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
		                        [conn](const std::unique_ptr<PyroStreamConnection> &ptr) {
			                        return ptr.get() == conn;
		                        });
		if (itr != connections.end())
			connections.erase(itr);
	}

	uint64_t cookie = 1000;
	std::mutex lock;
	std::vector<std::unique_ptr<PyroStreamConnection>> connections;
};

PyroStreamConnection::PyroStreamConnection(
		PyroFling::Dispatcher &dispatcher, PyroStreamConnectionCancelInterface &server_,
		const PyroFling::RemoteAddress &remote, uint64_t cookie_)
	: PyroFling::Handler(dispatcher)
	, server(server_)
	, tcp_remote(remote)
	, cookie(cookie_)
{
	packet_seq_video = cookie & ((1 << PYRO_PAYLOAD_PACKET_SEQ_BITS) - 1);
	packet_seq_audio = (~cookie) & ((1 << PYRO_PAYLOAD_PACKET_SEQ_BITS) - 1);
}

void PyroStreamConnection::set_codec_parameters(const pyro_codec_parameters &parameters)
{
	codec = parameters;
}

bool PyroStreamConnection::handle(const PyroFling::FileHandle &fd, uint32_t)
{
	// We've exhausted the buffer.
	if (tcp_length >= sizeof(tcp.buffer))
		return false;

	size_t ret = receive_stream_message(fd, tcp.buffer + tcp_length, sizeof(tcp.buffer) - tcp_length);
	if (!ret)
		return false;

	while (tcp_length && tcp_length >= sizeof(pyro_message_type) &&
	       tcp_length + sizeof(pyro_message_type) >= pyro_message_get_length(tcp.type))
	{
		if (!pyro_message_validate_magic(tcp.type))
			return false;

		switch (pyro_message_get_type(tcp.type))
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
			if (kicked)
				return false;

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
			break;

		default:
			// Invalid message.
			return false;
		}

		size_t move_len = pyro_message_get_length(tcp.type) + sizeof(pyro_message_type);
		memmove(tcp.buffer, tcp.buffer + move_len, tcp_length - move_len);
		tcp_length -= move_len;
	}

	return true;
}

void PyroStreamConnection::release_id(uint32_t)
{
	server.release_connection(this);
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
	uint32_t subseq = seq ^ 0xaabb; // Start on something arbitrary.

	auto *data = static_cast<const uint8_t *>(data_);
	for (size_t i = 0; i < size; i += PYRO_MAX_PAYLOAD_SIZE, data += PYRO_MAX_PAYLOAD_SIZE)
	{
		header.encoded &= ~(PYRO_PAYLOAD_PACKET_BEGIN_BIT | PYRO_PAYLOAD_PACKET_DONE_BIT);
		if (i == 0)
			header.encoded |= PYRO_PAYLOAD_PACKET_BEGIN_BIT;
		if (i + PYRO_MAX_PAYLOAD_SIZE >= size)
			header.encoded |= PYRO_PAYLOAD_PACKET_DONE_BIT;

		header.encoded &= ((1 << PYRO_PAYLOAD_SUBPACKET_SEQ_BITS) - 1) << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET;
		header.encoded |= subseq << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET;

		dispatcher.write_udp_datagram(udp_remote, &header, sizeof(header),
		                              data, std::min<size_t>(PYRO_MAX_PAYLOAD_SIZE, size - i));

		subseq = (subseq + 1) & ((1 << PYRO_PAYLOAD_SUBPACKET_SEQ_BITS) - 1);
	}

	seq = (seq + 1) & ((1 << PYRO_PAYLOAD_PACKET_SEQ_BITS) - 1);
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

	switch (pyro_message_get_type(type))
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

int main()
{
	using namespace PyroFling;
	Dispatcher::block_signals();
	Server server;

	Dispatcher dispatcher("/tmp/pyro", "8080");
	dispatcher.set_handler_factory_interface(&server);
	std::thread thr([&dispatcher]() { while (dispatcher.iterate()); });
	std::thread sender([&server]() {
		for (unsigned i = 0; i < 64; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	});

	dispatcher.kill();
	sender.join();
	thr.join();
}
