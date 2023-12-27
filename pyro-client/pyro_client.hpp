#pragma once
#include "pyro_protocol.h"
#include "simple_socket.hpp"
#include <stddef.h>
#include <vector>
#include <chrono>
#include <memory>
#include <stdio.h>

namespace PyroFling
{
class PyroStreamClient
{
public:
	bool connect(const char *host, const char *port);
	bool handshake(pyro_kick_state_flags flags);

	const pyro_codec_parameters &get_codec_parameters() const;
	bool wait_next_packet();

	const void *get_packet_data() const;
	size_t get_packet_size() const;
	const pyro_payload_header &get_payload_header() const;

	bool send_target_phase_offset(int offset_us);

	bool send_gamepad_state(const pyro_gamepad_state &state);

	// Purely for debugging.
	static void set_simulate_reordering(bool enable);
	static void set_simulate_drop(bool enable);
	void set_debug_log(const char *path);

	double get_current_ping_delay() const;

private:
	PyroFling::Socket tcp, udp;
	pyro_kick_state_flags kick_flags = 0;

	struct FileDeleter { void operator()(FILE *fp) { if (fp) fclose(fp); }};
	std::unique_ptr<FILE, FileDeleter> debug_log;

	void write_debug_header(const pyro_payload_header &header);

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
	uint16_t gamepad_seq = 0;
	uint16_t ping_seq = 0;
	uint64_t ping_times[256] = {};
	double last_ping_delay = 0.0;

	bool iterate();

	bool check_send_progress();
};
}