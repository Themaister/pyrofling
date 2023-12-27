#include "pyro_client.hpp"
#include "timer.hpp"
#include <string.h>
#include <random>

namespace PyroFling
{
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
	if (!debug_log)
		return;

	uint32_t packet_seq = pyro_payload_get_packet_seq(header.encoded);
	uint32_t packet_subseq = pyro_payload_get_subpacket_seq(header.encoded);
	bool packet_key = (header.encoded & PYRO_PAYLOAD_KEY_FRAME_BIT) != 0;
	bool stream_type = (header.encoded & PYRO_PAYLOAD_STREAM_TYPE_BIT) != 0;
	bool packet_begin = (header.encoded & PYRO_PAYLOAD_PACKET_BEGIN_BIT) != 0;
	bool packet_done = (header.encoded & PYRO_PAYLOAD_PACKET_DONE_BIT) != 0;

	fprintf(debug_log.get(), "SEQ %04x | SUBSEQ %04x | KEY %d | TYPE %d |%s%s\n",
	        packet_seq, packet_subseq, int(packet_key), int(stream_type), packet_begin ? " [BEGIN]" : "",
	        packet_done ? " [DONE] " : "");
}

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
			if (sim.header.encoded & PYRO_PAYLOAD_PACKET_DONE_BIT)
				printf(" [DONE]");
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
			if (payload.header.encoded & PYRO_PAYLOAD_PACKET_DONE_BIT)
				printf(" [DONE]");
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

	// Partial packets must be full packets (for simplicity to avoid stitching payloads together).
	if ((payload.header.encoded & PYRO_PAYLOAD_PACKET_DONE_BIT) == 0 && payload.size != PYRO_MAX_PAYLOAD_SIZE)
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
		stream->buffer.resize(stream->subpacket_seq_accum * PYRO_MAX_PAYLOAD_SIZE + payload.size);
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
		       payload.buffer, payload.size);
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

			if (debug_log && delta > 1)
				fprintf(debug_log.get(), "DROP\n");
			progress.total_dropped_packets += delta - 1;
		}

		last_completed_seq = stream->packet_seq;
		stream->payload = payload.header;
		progress.total_received_packets++;

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
}