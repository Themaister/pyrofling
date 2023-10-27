#include "pyro_client.hpp"
#include <string.h>

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
}