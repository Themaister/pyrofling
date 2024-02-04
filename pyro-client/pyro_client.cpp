#include "pyro_client.hpp"
#include "timer.hpp"
#include <string.h>
#include <random>
#include <assert.h>

namespace PyroFling
{
void ReconstructedPacket::reset()
{
	buffer.clear();
	last_subpacket_raw_seq = 0;
	subpacket_seq_accum = 0;
	packet_seq = 0;
}

bool ReconstructedPacket::is_fec_recovered() const
{
	return fec_recovered;
}

void ReconstructedPacket::prepare_decode(const pyro_payload_header &header)
{
	// Copy payload data into correct offset.
	uint32_t subpacket_seq = pyro_payload_get_subpacket_seq(header.encoded);

	if (buffer.empty())
	{
		current_header = header;
		is_done = false;

		// Set a reasonable upper bound.
		size_t buffer_size = (header.payload_size + PYRO_MAX_PAYLOAD_SIZE - 1) & ~(PYRO_MAX_PAYLOAD_SIZE - 1);
		buffer_size = std::min<size_t>(buffer_size, 128 * 1024 * PYRO_MAX_PAYLOAD_SIZE);
		buffer.resize(buffer_size);

		// Bound by 16-bit FEC count.
		fec_buffer.resize(header.num_fec_blocks * PYRO_MAX_PAYLOAD_SIZE);

		decoder.set_block_size(PYRO_MAX_PAYLOAD_SIZE);
		decoder.begin_decode(header.pts_lo, buffer.data(), buffer.size(), header.num_fec_blocks,
		                     header.num_xor_blocks_even, header.num_xor_blocks_odd);

		subpacket_seq_accum = 0;
		last_subpacket_raw_seq = 0;
		fec_recovered = false;
	}

	if ((header.encoded & PYRO_PAYLOAD_PACKET_FEC_BIT) == 0)
	{
		subpacket_seq_accum += pyro_payload_get_subpacket_seq_delta(subpacket_seq, last_subpacket_raw_seq);
		last_subpacket_raw_seq = subpacket_seq;

		// Error: we received bogus out-of-order sequences.
		// Check: subsequence 0 must have a BEGIN flag.
		// Check: subsequence != 0 must not have a BEGIN flag.
		if (subpacket_seq_accum < 0 ||
		    (subpacket_seq_accum == 0 && (header.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT) == 0) ||
		    (subpacket_seq_accum != 0 && (header.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT) != 0))
		{
			is_error = true;
		}
	}
}

bool ReconstructedPacket::is_reset() const
{
	return buffer.empty();
}

bool ReconstructedPacket::is_complete() const
{
	return is_done && !is_error;
}

const pyro_payload_header &ReconstructedPacket::get_payload_header() const
{
	return current_header;
}

const void *ReconstructedPacket::get_packet_data() const
{
	return buffer.data();
}

size_t ReconstructedPacket::get_packet_size() const
{
	return current_header.payload_size;
}

void ReconstructedPacket::add_payload_data(const void *data, size_t size)
{
	if (is_done || is_error)
		return;

	size_t offset = subpacket_seq_accum * PYRO_MAX_PAYLOAD_SIZE;
	if (offset < buffer.size())
	{
		memcpy(buffer.data() + offset, data, size);
		if (size != PYRO_MAX_PAYLOAD_SIZE)
			memset(buffer.data() + offset + size, 0, PYRO_MAX_PAYLOAD_SIZE - size);

		is_done = decoder.push_raw_block(subpacket_seq_accum);
	}
}

void ReconstructedPacket::add_fec_data(uint32_t subseq, const void *data, size_t size)
{
	if (is_done || is_error)
		return;

	size_t offset = subseq * PYRO_MAX_PAYLOAD_SIZE;
	if (offset < fec_buffer.size())
	{
		memcpy(fec_buffer.data() + offset, data, size);
		is_done = decoder.push_fec_block(subseq, fec_buffer.data() + offset);
		if (is_done)
			fec_recovered = true;
	}
}

bool PyroStreamClient::connect(const char *host, const char *port)
{
	if (!tcp.connect(PyroFling::Socket::Proto::TCP, host, port))
		return false;
	if (!udp.connect(PyroFling::Socket::Proto::UDP, host, port))
		return false;

	return true;
}

bool PyroStreamClient::handshake(pyro_kick_state_flags flags)
{
	pyro_message_type type = PYRO_MESSAGE_HELLO;
	if (!tcp.write(&type, sizeof(type)))
		return false;

	if (!tcp.read(&type, sizeof(type)) || type != PYRO_MESSAGE_COOKIE)
		return false;

	uint64_t cookie = 0;
	if (!tcp.read(&cookie, sizeof(cookie)))
		return false;

	for (unsigned i = 0; i < 64 && codec.video_codec == PYRO_VIDEO_CODEC_NONE; i++)
	{
		type = PYRO_MESSAGE_COOKIE;
		if (!udp.write_message(&type, sizeof(type), &cookie, sizeof(cookie)))
			return false;

		type = PYRO_MESSAGE_KICK;
		if (!tcp.write(&type, sizeof(type)))
			return false;
		if (!tcp.write(&flags, sizeof(flags)))
			return false;

		if (!tcp.read(&type, sizeof(type)))
			return false;
		if (type != PYRO_MESSAGE_CODEC_PARAMETERS)
			continue;
		if (!tcp.read(&codec, sizeof(codec)))
			return false;
	}

	last_progress_time = std::chrono::steady_clock::now();
	kick_flags = flags;
	return codec.video_codec != PYRO_VIDEO_CODEC_NONE;
}

const void *PyroStreamClient::get_packet_data() const
{
	return current ? current->get_packet_data() : nullptr;
}

size_t PyroStreamClient::get_packet_size() const
{
	return current ? current->get_packet_size() : 0;
}

const pyro_codec_parameters &PyroStreamClient::get_codec_parameters() const
{
	return codec;
}

const pyro_payload_header &PyroStreamClient::get_payload_header() const
{
	return current->get_payload_header();
}

bool PyroStreamClient::send_target_phase_offset(int offset_us)
{
	pyro_message_type type = PYRO_MESSAGE_PHASE_OFFSET;
	int32_t data = offset_us;
	return udp.write_message(&type, sizeof(type), &data, sizeof(data));
}

double PyroStreamClient::get_current_ping_delay() const
{
	return last_ping_delay;
}

bool PyroStreamClient::send_gamepad_state(const pyro_gamepad_state &state)
{
	auto send_state = state;
	send_state.seq = gamepad_seq++;
	pyro_message_type type = PYRO_MESSAGE_GAMEPAD_STATE;
	if (!udp.write_message(&type, sizeof(type), &send_state, sizeof(send_state)))
		return false;

	// Send regular ping requests to measure round-trip delay.
	if ((gamepad_seq & 15) == 0)
	{
		type = PYRO_MESSAGE_PING;
		pyro_ping_state ping_state = {};
		ping_state.seq = ping_seq++ % 256;
		if (!udp.write_message(&type, sizeof(type), &ping_state, sizeof(ping_state)))
			return false;
		ping_times[ping_state.seq] = Util::get_current_time_nsecs();
	}

	// If we're a pure gamepad connection, need to keep-alive here.
	if ((kick_flags & (PYRO_KICK_STATE_AUDIO_BIT | PYRO_KICK_STATE_VIDEO_BIT)) == 0 && !check_send_progress())
		return false;

	return true;
}

struct Packet
{
	pyro_payload_header header;
	uint8_t buffer[PYRO_MAX_PAYLOAD_SIZE];
	uint32_t size;
};

//#define PYRO_DEBUG_REORDER

#ifdef PYRO_DEBUG_REORDER
static Packet simulate_packets[8];
unsigned num_simulated_packets;
static bool simulate_drop;
static bool simulate_reordering;
static std::default_random_engine rng{100};
static constexpr int PYRO_ROBUST_DIVIDER = 256 * 1024;

void PyroStreamClient::set_simulate_drop(bool enable)
{
	simulate_drop = enable;
}

void PyroStreamClient::set_simulate_reordering(bool enable)
{
	simulate_reordering = enable;
}
#else
void PyroStreamClient::set_simulate_drop(bool)
{
}

void PyroStreamClient::set_simulate_reordering(bool)
{
}
#endif

void PyroStreamClient::set_debug_log(const char *path)
{
	debug_log.reset(fopen(path, "w"));
}

void PyroStreamClient::write_debug_header(const pyro_payload_header &header)
{
	bool stream_type = (header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) != 0;
	if (stream_type || !debug_log)
		return;

	uint32_t packet_seq = pyro_payload_get_packet_seq(header.encoded);
	uint32_t packet_subseq = pyro_payload_get_subpacket_seq(header.encoded);
	bool packet_key = (header.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT) != 0;
	bool packet_begin = (header.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT) != 0;
	bool packet_fec = (header.encoded & PYRO_PAYLOAD_PACKET_FEC_BIT) != 0;

	uint32_t num_packets = (header.payload_size + PYRO_MAX_PAYLOAD_SIZE - 1) / PYRO_MAX_PAYLOAD_SIZE;

	auto current_t = std::chrono::steady_clock::now();
	auto delta_t = std::chrono::steady_clock::now() - base_time;
	auto millisecs = double(std::chrono::duration_cast<std::chrono::nanoseconds>(delta_t).count()) * 1e-6;
	base_time = current_t;

	fprintf(debug_log.get(), "T delta = %8.3f ms | SIZE %06u | SEQ %04x | SUBSEQ %u / %u | KEY %d | TYPE %s |%s%s\n",
			millisecs,
			header.payload_size,
	        packet_seq, packet_subseq, num_packets,
			int(packet_key), stream_type ? "AUDIO" : "VIDEO", packet_begin ? " [BEGIN]" : "",
	        packet_fec ? " [FEC] " : "");
}

ReconstructedPacket *
PyroStreamClient::get_stream_packet(ReconstructedPacket *stream_base, uint32_t packet_seq)
{
	unsigned num_active_packets = 0;
	if (!stream_base[0].is_reset())
	{
		num_active_packets++;
		if (!stream_base[1].is_reset())
			num_active_packets++;
	}
	else
		assert(stream_base[1].is_reset());

	ReconstructedPacket *stream = nullptr;

	if (num_active_packets == 0)
	{
		// Trivial case, start a new packet.
		stream = &stream_base[0];
		stream->reset();
		stream->packet_seq = packet_seq;
		return stream;
	}

	for (unsigned i = 0; i < num_active_packets && !stream; i++)
		if (!stream_base[i].is_reset() && stream_base[i].packet_seq == packet_seq)
			stream = &stream_base[i];

	if (stream)
		return stream;

	// Need to start a new stream. Figure out where to insert it.
	unsigned i;
	for (i = 0; i < num_active_packets; i++)
		if (pyro_payload_get_packet_seq_delta(packet_seq, stream_base[i].packet_seq) < 0)
			break;

	bool swap_packets = false;

	if (i == 0)
	{
		if (num_active_packets == 1)
			swap_packets = true;
		else
			return nullptr;
	}
	else if (i == 1)
	{
		if (num_active_packets == 2)
		{
			// Age out packet[0]
			i = 0;
		}
	}
	else if (i == 2)
	{
		// Age out packet[0].
		swap_packets = true;
		i = 1;
	}

	if (swap_packets)
		std::swap(stream_base[0], stream_base[1]);

	stream = &stream_base[i];

	if (stream)
	{
		stream->reset();
		stream->packet_seq = packet_seq;
	}

	return stream;
}

#define LOG(...) if (!is_audio && debug_log) fprintf(debug_log.get(), __VA_ARGS__)

bool PyroStreamClient::iterate()
{
	Packet payload;

#ifdef PYRO_DEBUG_REORDER
	if (simulate_drop || simulate_reordering)
	{
		auto &sim = simulate_packets[num_simulated_packets];
		sim.size = udp.read_partial(&sim, sizeof(sim), &tcp);
		if (sim.size <= sizeof(pyro_payload_header) || sim.size > PYRO_MAX_UDP_DATAGRAM_SIZE)
			return false;

		if ((payload.header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) == 0)
		{
			printf(" SIM Received [%u, %u]",
			       pyro_payload_get_packet_seq(sim.header.encoded),
			       pyro_payload_get_subpacket_seq(sim.header.encoded));
			if (sim.header.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT)
				printf(" [BEGIN]");
			if (sim.header.encoded & PYRO_PAYLOAD_PACKET_FEC_BIT)
				printf(" [FEC]");
			if (sim.header.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT)
				printf(" [KEY]");
			printf("\n");
		}

		if (simulate_drop)
		{
#if 0
			if (rng() % PYRO_ROBUST_DIVIDER == 0)
			{
				printf(" !! Dropped [%u, %u]\n",
				       pyro_payload_get_packet_seq(sim.header.encoded),
				       pyro_payload_get_subpacket_seq(sim.header.encoded));
			}
			else
#endif
				num_simulated_packets++;
		}
		else
			num_simulated_packets++;

#if 0
		if (num_simulated_packets >= 2 && simulate_reordering && rng() % PYRO_ROBUST_DIVIDER == 0)
		{
			unsigned index0 = rng() % num_simulated_packets;
			unsigned index1 = rng() % num_simulated_packets;
			if (index0 != index1)
			{
				printf(" !! Reordered [%u, %u] <-> [%u, %u]\n",
				       pyro_payload_get_packet_seq(simulate_packets[0].header.encoded),
				       pyro_payload_get_subpacket_seq(simulate_packets[0].header.encoded),
				       pyro_payload_get_packet_seq(simulate_packets[1].header.encoded),
				       pyro_payload_get_subpacket_seq(simulate_packets[1].header.encoded));
				std::swap(simulate_packets[index0], simulate_packets[index1]);
			}
		}
#endif

		if (num_simulated_packets >= 1)
		{
			memcpy(&payload, &simulate_packets[0], sizeof(payload));
			num_simulated_packets--;
			memmove(&simulate_packets[0], &simulate_packets[1],
					num_simulated_packets * sizeof(simulate_packets[0]));
		}
		else
			return true;

		if ((payload.header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) == 0)
		{
			printf(" Received [%u, %u]",
			       pyro_payload_get_packet_seq(payload.header.encoded),
			       pyro_payload_get_subpacket_seq(payload.header.encoded));
			if (payload.header.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT)
				printf(" [BEGIN]");
			if (payload.header.encoded & PYRO_PAYLOAD_PACKET_FEC_BIT)
				printf(" [FEC]");
			if (payload.header.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT)
				printf(" [KEY]");
			printf("\n");
		}
	}
	else
#endif
	{
		payload.size = udp.read_partial(&payload, sizeof(payload), &tcp);
	}

	if (payload.size < sizeof(pyro_payload_header) || payload.size > PYRO_MAX_UDP_DATAGRAM_SIZE)
		return false;
	payload.size -= sizeof(pyro_payload_header);

	write_debug_header(payload.header);

	// Only special packet currently supported is a PING.
	const auto special_packet = PYRO_PAYLOAD_KEY_FRAME_BIT | PYRO_PAYLOAD_STREAM_TYPE_BIT;
	if ((payload.header.encoded & special_packet) == special_packet)
	{
		uint32_t packet_seq = pyro_payload_get_packet_seq(payload.header.encoded);
		last_ping_delay = 1e-9 * double(Util::get_current_time_nsecs() - ping_times[packet_seq % 256]);
		return true;
	}

	bool is_audio = (payload.header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) != 0;

	auto *stream_base = is_audio ? &audio[0] : &video[0];
	auto &last_completed_seq = is_audio ? last_completed_audio_seq : last_completed_video_seq;
	auto &h = payload.header;

	if ((h.encoded & PYRO_PAYLOAD_PACKET_FEC_BIT) != 0 && is_audio)
	{
		LOG("  invalid fec\n");
		return true;
	}

	uint32_t packet_seq = pyro_payload_get_packet_seq(h.encoded);

	// Either we work on an existing packet,
	// drop the packet if it's too old, or discard existing packets if we start receiving subpackets that obsolete
	// the existing packet.

	// Principle of the implementation is to commit to a packet when it has been completed.
	// Only allow one packet to be received out of order.
	// Only retire packets monotonically.

	// Duplicate packets most likely, or very old packets were sent.
	if (last_completed_seq != UINT32_MAX && pyro_payload_get_packet_seq_delta(packet_seq, last_completed_seq) <= 0)
	{
		LOG("  old packet\n");
		return true;
	}

	auto *stream = get_stream_packet(stream_base, packet_seq);

	if (!stream)
	{
		LOG("  old packet\n");
		return true;
	}

	LOG("  packet[%d]\n", int(stream - stream_base));

	stream->prepare_decode(h);

	if ((h.encoded & PYRO_PAYLOAD_PACKET_FEC_BIT) != 0)
	{
		// FEC blocks must be MAX_PAYLOAD_SIZE.
		if (payload.size != PYRO_MAX_PAYLOAD_SIZE)
		{
			LOG("  invalid fec size\n");
			return false;
		}

		uint32_t subpacket_seq = pyro_payload_get_subpacket_seq(h.encoded);
		stream->add_fec_data(subpacket_seq, payload.buffer, payload.size);
	}
	else
	{
		stream->add_payload_data(payload.buffer, payload.size);
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

		LOG("  complete seq %04x\n", packet_seq);

		if (last_completed_seq != UINT32_MAX)
		{
			int delta = pyro_payload_get_packet_seq_delta(stream->packet_seq, last_completed_seq);

			if (delta < 1)
			{
				// Bogus case. Something has gone very wrong!
				LOG("  invalid packet seq delta %d\n", delta);
				return false;
			}

			if (delta > 1)
				LOG("  %d packet drops\n", delta - 1);
			progress.total_dropped_packets += delta - 1;
		}

		last_completed_seq = stream->packet_seq;
		progress.total_received_packets++;

		if (stream->is_fec_recovered())
		{
			LOG("  recovered seq %x with fec\n", stream->packet_seq);
			progress.total_recovered_packets++;
		}

#ifdef PYRO_DEBUG_REORDER
		if ((h.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) == 0)
		{
			printf(" !! COMPLETE VIDEO %u %s\n", last_completed_seq,
			       (h.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT) ? "[KEY]" : "");
		}
#endif

		if ((h.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT) != 0)
			progress.total_received_key_frames++;

		if (!check_send_progress())
			return false;
		current = stream;
	}

	return true;
}

bool PyroStreamClient::check_send_progress()
{
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

PyroStreamClient::PyroStreamClient()
{
	base_time = std::chrono::steady_clock::now();
}
}