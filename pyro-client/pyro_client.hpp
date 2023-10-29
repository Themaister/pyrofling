#pragma once
#include "pyro_protocol.h"
#include "simple_socket.hpp"
#include <stddef.h>
#include <vector>
#include <chrono>

namespace PyroFling
{
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

	bool send_target_phase_offset(int offset_us);

	// Purely for debugging.
	static void set_simulate_reordering(bool enable);
	static void set_simulate_drop(bool enable);

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
	bool has_observed_keyframe = false;

	bool iterate();
};
}