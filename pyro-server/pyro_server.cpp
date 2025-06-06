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

	needs_key_frame.store(false, std::memory_order_relaxed);
	has_pending_video_packet_loss.store(false, std::memory_order_relaxed);
}

bool PyroStreamConnection::requires_idr()
{
	return (kick_flags & PYRO_KICK_STATE_VIDEO_BIT) != 0 && needs_key_frame.load(std::memory_order_relaxed);
}

void PyroStreamConnection::set_forward_error_correction(bool enable)
{
	fec = enable;
}

bool PyroStreamConnection::get_and_clear_pending_video_packet_loss()
{
	return has_pending_video_packet_loss.exchange(false, std::memory_order_relaxed);
}

bool PyroStreamConnection::handle(const PyroFling::FileHandle &fd, uint32_t id)
{
	// Timeout, cancel everything.
	if (id)
	{
		printf("TIMEOUT for %s @ %s\n", remote_addr.c_str(), remote_port.c_str());
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
			memcpy(&kick_flags, tcp.split.payload, sizeof(kick_flags));

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
				needs_key_frame.store(true, std::memory_order_relaxed);
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
			tv.it_value.tv_sec = 15;
			timerfd_settime(timer_fd.get_native_handle(), 0, &tv, nullptr);

			break;
		}

		case PYRO_MESSAGE_PROGRESS:
		{
			// Re-start timeout.
			struct itimerspec tv = {};
			tv.it_value.tv_sec = 15;
			timerfd_settime(timer_fd.get_native_handle(), 0, &tv, nullptr);
			memcpy(&progress, tcp.split.payload, sizeof(progress));

			if ((kick_flags & (PYRO_KICK_STATE_AUDIO_BIT | PYRO_KICK_STATE_VIDEO_BIT)) != 0)
			{
				printf("PROGRESS for %s @ %s: %llu complete, %llu dropped video, %llu dropped audio, %llu key frames, %llu FEC recovered.\n",
				       remote_addr.c_str(), remote_port.c_str(),
				       static_cast<unsigned long long>(progress.total_received_packets),
				       static_cast<unsigned long long>(progress.total_dropped_video_packets),
				       static_cast<unsigned long long>(progress.total_dropped_audio_packets),
				       static_cast<unsigned long long>(progress.total_received_key_frames),
					   static_cast<unsigned long long>(progress.total_recovered_packets));
			}

			needs_key_frame.store(progress.total_received_key_frames == 0, std::memory_order_relaxed);
			if (total_dropped_video_packets != progress.total_dropped_video_packets)
				has_pending_video_packet_loss.store(true, std::memory_order_relaxed);
			total_dropped_video_packets = progress.total_dropped_video_packets;
			break;
		}

		default:
		{
			const pyro_message_type type = PYRO_MESSAGE_NAK;
			if (!send_stream_message(fd, &type, sizeof(type)))
				return false;
			break;
		}
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

	if (is_audio && (kick_flags & PYRO_KICK_STATE_AUDIO_BIT) == 0)
		return;
	if (!is_audio && (kick_flags & PYRO_KICK_STATE_VIDEO_BIT) == 0)
		return;

	auto &seq = is_audio ? packet_seq_audio : packet_seq_video;

	// ~25% FEC overhead. TODO: Make configurable.
	uint32_t num_data_blocks = (size + PYRO_MAX_PAYLOAD_SIZE - 1) / PYRO_MAX_PAYLOAD_SIZE;
	uint32_t num_fec_blocks = num_data_blocks / 4 + 1;
	uint32_t num_xor_blocks_even = std::min<uint32_t>(num_data_blocks / 2, 64u);
	uint32_t num_xor_blocks_odd = std::min<uint32_t>((num_data_blocks + 1) / 2, 64u);

	// For small packets, just send a single full-XOR FEC block which can recover exactly one error.
	if (num_data_blocks <= 8)
	{
		num_xor_blocks_even = num_data_blocks;
		num_xor_blocks_odd = num_data_blocks;
		num_fec_blocks = 1;
	}

	pyro_payload_header header = {};
	header.pts_lo = uint32_t(pts);
	header.pts_hi = uint32_t(pts >> 32);
	header.dts_delta = uint32_t(pts - dts);
	header.encoded |= is_audio ? PYRO_PAYLOAD_STREAM_TYPE_BIT : 0;
	header.encoded |= is_key_frame ? PYRO_PAYLOAD_KEY_FRAME_BIT : 0;
	header.encoded |= seq << PYRO_PAYLOAD_PACKET_SEQ_OFFSET;
	header.payload_size = size;

	if (!is_audio && fec)
	{
		header.num_xor_blocks_even = num_xor_blocks_even;
		header.num_xor_blocks_odd = num_xor_blocks_odd;
		header.num_fec_blocks = num_fec_blocks;
	}

	uint32_t subseq = 0;

	auto *data = static_cast<const uint8_t *>(data_);
	for (size_t i = 0; i < size; i += PYRO_MAX_PAYLOAD_SIZE)
	{
		header.encoded &= ~PYRO_PAYLOAD_PACKET_BEGIN_BIT;
		if (i == 0)
			header.encoded |= PYRO_PAYLOAD_PACKET_BEGIN_BIT;

		header.encoded &= ~(PYRO_PAYLOAD_SUBPACKET_SEQ_MASK << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET);
		header.encoded |= subseq << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET;

		if (dispatcher.write_udp_datagram(udp_remote, &header, sizeof(header),
		                                  data + i, std::min<size_t>(PYRO_MAX_PAYLOAD_SIZE, size - i)) < 0)
		{
			fprintf(stderr, "Error writing UDP datagram. Congested buffers?\n");
		}

		subseq = (subseq + 1) & PYRO_PAYLOAD_SUBPACKET_SEQ_MASK;
	}

	if (!is_audio && fec)
	{
		uint8_t xor_data[PYRO_MAX_PAYLOAD_SIZE];

		header.encoded &= ~PYRO_PAYLOAD_PACKET_BEGIN_BIT;
		header.encoded |= PYRO_PAYLOAD_PACKET_FEC_BIT;

		encoder.flush();
		encoder.seed(header.pts_lo);
		encoder.set_block_size(PYRO_MAX_PAYLOAD_SIZE);

		for (uint32_t i = 0; i < num_fec_blocks; i++)
		{
			encoder.generate(xor_data, data, size, i & 1 ? num_xor_blocks_odd : num_xor_blocks_even);

			header.encoded &= ~(PYRO_PAYLOAD_SUBPACKET_SEQ_MASK << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET);
			header.encoded |= i << PYRO_PAYLOAD_SUBPACKET_SEQ_OFFSET;

			if (dispatcher.write_udp_datagram(
					udp_remote, &header, sizeof(header),
					xor_data, sizeof(xor_data)) < 0)
			{
				fprintf(stderr, "Error writing UDP datagram. Congested buffers?\n");
			}
		}
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
		PyroFling::Dispatcher &dispatcher_, const PyroFling::RemoteAddress &remote,
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

	case PYRO_MESSAGE_PHASE_OFFSET:
	{
		if (udp_remote == remote)
		{
			pyro_phase_offset phase = {};
			memcpy(&phase, msg, sizeof(phase));
			server.set_phase_offset(phase.ideal_phase_offset_us);
		}
		break;
	}

	case PYRO_MESSAGE_GAMEPAD_STATE:
	{
		if (udp_remote == remote && (kick_flags & PYRO_KICK_STATE_GAMEPAD_BIT) != 0)
		{
			pyro_gamepad_state state = {};
			memcpy(&state, msg, sizeof(state));

			// Only accept monotonic gamepad updates.
			if (((state.seq - last_gamepad_seq) & 0x8000) == 0 || !valid_gamepad_seq)
			{
				server.set_gamepad_state(udp_remote, state);
				last_gamepad_seq = state.seq;
			}
			valid_gamepad_seq = true;
		}
		break;
	}

	case PYRO_MESSAGE_PING:
	{
		if (udp_remote == remote && kicked)
		{
			pyro_ping_state state = {};
			memcpy(&state, msg, sizeof(state));
			state.seq &= PYRO_PAYLOAD_PACKET_SEQ_MASK;
			pyro_payload_header header = {};
			header.encoded |= PYRO_PAYLOAD_KEY_FRAME_BIT | PYRO_PAYLOAD_STREAM_TYPE_BIT;
			header.encoded |= state.seq << PYRO_PAYLOAD_PACKET_SEQ_OFFSET;
			dispatcher_.write_udp_datagram(udp_remote, &header, sizeof(header), nullptr, 0);
		}
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
	conn->set_forward_error_correction(fec);
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
	// Rate limit forced IDR frames to avoid overwhelming the encoder and bandwidth.
	if (idr_counter++ < 60)
		return false;

	bool requires_idr = false;

	std::lock_guard<std::mutex> holder{lock};
	for (auto &conn : connections)
	{
		bool has_pending_packet_loss = conn->get_and_clear_pending_video_packet_loss();
		if ((has_pending_packet_loss && idr_on_packet_loss) || conn->requires_idr())
			requires_idr = true;
	}

	if (requires_idr)
		idr_counter = 0;
	return requires_idr;
}

PyroStreamServer::PyroStreamServer()
{
	phase_offset_us.store(0, std::memory_order_relaxed);
}

void PyroStreamServer::set_phase_offset(int phase_offset_us_)
{
	phase_offset_us.fetch_add(phase_offset_us_, std::memory_order_relaxed);
}

int PyroStreamServer::get_phase_offset_us() const
{
	return phase_offset_us.exchange(0, std::memory_order_relaxed);
}

const pyro_gamepad_state *PyroStreamServer::get_updated_gamepad_state()
{
	auto *ret = new_gamepad_state ? &current_gamepad_state : nullptr;
	new_gamepad_state = false;
	return ret;
}

void PyroStreamServer::set_forward_error_correction(bool enable)
{
	fec = enable;
}

void PyroStreamServer::set_idr_on_packet_loss(bool enable)
{
	idr_on_packet_loss = enable;
}

void PyroStreamServer::set_gamepad_state(const RemoteAddress &remote, const pyro_gamepad_state &state)
{
	// Use mode bit to take control of session. Super crude, but good enough for POC.
	bool button_combo_takes_control = (state.buttons & PYRO_PAD_MODE_BIT) != 0;
	constexpr uint16_t button_combo =
			PYRO_PAD_START_BIT | PYRO_PAD_SELECT_BIT |
			PYRO_PAD_TL_BIT | PYRO_PAD_TR_BIT;
	if ((state.buttons & button_combo) == button_combo)
		button_combo_takes_control = true;

	if (remote == current_gamepad_remote || !current_gamepad_remote || button_combo_takes_control)
	{
		current_gamepad_state = state;
		current_gamepad_remote = remote;
		new_gamepad_state = true;
	}
}
}
