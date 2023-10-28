#include "pyro_server.hpp"
#include "messages.hpp"
#include <algorithm>

#include <sys/timerfd.h>
#include <string.h>
#include <netdb.h>

namespace PyroFling
{
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

	char host[1024];
	char serv[1024];
	if (getnameinfo(reinterpret_cast<const struct sockaddr *>(&tcp_remote.addr), tcp_remote.addr_size,
	                host, sizeof(host), serv, sizeof(serv), 0) == 0)
	{
		printf("REMOTE: %s @ %s\n", host, serv);
		remote_addr = host;
		remote_port = serv;
	}

	has_observed_keyframe = false;
}

bool PyroStreamConnection::requires_idr()
{
	return !has_observed_keyframe.load(std::memory_order_relaxed);
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
			printf("HELLO for %s @ %s\n", remote_addr.c_str(), remote_port.c_str());
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
			{
				printf("REDUNDANT KICK for %s @ %s\n", remote_addr.c_str(), remote_port.c_str());
				return true;
			}

			auto codec = server.get_codec_parameters();

			if (udp_remote && codec.video_codec != PYRO_VIDEO_CODEC_NONE)
			{
				printf("KICK -> OK for %s @ %s\n", remote_addr.c_str(), remote_port.c_str());
				const pyro_message_type type = PYRO_MESSAGE_CODEC_PARAMETERS;
				if (!send_stream_message(fd, &type, sizeof(type)))
					return false;
				if (!send_stream_message(fd, &codec, sizeof(codec)))
					return false;
				kicked = true;
			}
			else if (udp_remote)
			{
				printf("KICK -> AGAIN for %s @ %s\n", remote_addr.c_str(), remote_port.c_str());
				const pyro_message_type type = PYRO_MESSAGE_AGAIN;
				if (!send_stream_message(fd, &type, sizeof(type)))
					return false;
			}
			else
			{
				printf("KICK -> NAK for %s @ %s\n", remote_addr.c_str(), remote_port.c_str());
				const pyro_message_type type = PYRO_MESSAGE_NAK;
				if (!send_stream_message(fd, &type, sizeof(type)))
					return false;
			}

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

			printf("PROGRESS for %s @ %s: %llu complete, %llu dropped, %llu key frames.\n",
			       remote_addr.c_str(), remote_port.c_str(),
			       static_cast<unsigned long long>(progress.total_received_packets),
			       static_cast<unsigned long long>(progress.total_dropped_packets),
			       static_cast<unsigned long long>(progress.total_received_key_frames));

			if (progress.total_received_key_frames)
				has_observed_keyframe.store(true, std::memory_order_relaxed);

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

		if (dispatcher.write_udp_datagram(udp_remote, &header, sizeof(header),
		                                  data, std::min<size_t>(PYRO_MAX_PAYLOAD_SIZE, size - i)) < 0)
		{
			fprintf(stderr, "Error writing UDP datagram. Congested buffers?\n");
		}

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
		char host[1024];
		char serv[1024];
		if (getnameinfo(reinterpret_cast<const struct sockaddr *>(&remote.addr), remote.addr_size,
		                host, sizeof(host), serv, sizeof(serv), 0) == 0)
		{
			printf("UDP COOKIE for %s @ %s : %s @ %s\n",
			       remote_addr.c_str(), remote_port.c_str(),
			       host, serv);
		}
		if (memcmp(msg, &cookie, sizeof(cookie)) == 0 && !udp_remote)
			udp_remote = remote;
		break;
	}

	default:
		break;
	}
}

void PyroStreamServer::set_codec_parameters(const pyro_codec_parameters &codec_)
{
	std::lock_guard<std::mutex> holder{lock};
	codec = codec_;
}

pyro_codec_parameters PyroStreamServer::get_codec_parameters()
{
	std::lock_guard<std::mutex> holder{lock};
	return codec;
}

bool PyroStreamServer::register_tcp_handler(PyroFling::Dispatcher &dispatcher, const PyroFling::FileHandle &,
                                            const PyroFling::RemoteAddress &remote,
                                            PyroFling::Handler *&handler)
{
	auto conn = Util::make_handle<PyroStreamConnection>(dispatcher, *this, remote, ++cookie);
	conn->add_reference();
	handler = conn.get();
	std::lock_guard<std::mutex> holder{lock};
	connections.push_back(std::move(conn));
	return true;
}

void PyroStreamServer::write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame)
{
	std::lock_guard<std::mutex> holder{lock};
	for (auto &conn : connections)
		conn->write_video_packet(pts, dts, data, size, is_key_frame);
}

void PyroStreamServer::write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size)
{
	std::lock_guard<std::mutex> holder{lock};
	for (auto &conn : connections)
		conn->write_audio_packet(pts, dts, data, size);
}

void PyroStreamServer::handle_udp_datagram(PyroFling::Dispatcher &dispatcher, const PyroFling::RemoteAddress &remote,
                                           const void *msg, unsigned size)
{
	for (auto &conn : connections)
		conn->handle_udp_datagram(dispatcher, remote, msg, size);
}

void PyroStreamServer::release_connection(PyroStreamConnection *conn)
{
	std::lock_guard<std::mutex> holder{lock};
	auto itr = std::find_if(connections.begin(), connections.end(),
	                        [conn](const Util::IntrusivePtr<PyroStreamConnection> &ptr) {
		                        return ptr.get() == conn;
	                        });
	if (itr != connections.end())
		connections.erase(itr);
}

bool PyroStreamServer::should_force_idr()
{
	bool requires_idr = false;
	if (idr_counter++ < 60)
		return false;
	idr_counter = 0;

	std::lock_guard<std::mutex> holder{lock};
	for (auto &conn : connections)
	{
		if (conn->requires_idr())
		{
			requires_idr = true;
			break;
		}
	}

	return requires_idr;
}
}