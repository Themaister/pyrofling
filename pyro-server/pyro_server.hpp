#pragma once
#include "pyro_protocol.h"
#include "listener.hpp"
#include "intrusive.hpp"
#include <atomic>
#include <mutex>

namespace PyroFling
{
class PyroStreamConnection;

class PyroStreamConnectionServerInterface
{
public:
	virtual ~PyroStreamConnectionServerInterface() = default;
	virtual void release_connection(PyroStreamConnection *conn) = 0;
	virtual pyro_codec_parameters get_codec_parameters() = 0;
	virtual void set_phase_offset(int phase_us) = 0;
	virtual void set_gamepad_state(const RemoteAddress &remote, const pyro_gamepad_state &state) = 0;
};

class PyroStreamConnection : public PyroFling::Handler, public Util::ThreadSafeIntrusivePtrEnabled<PyroStreamConnection>
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

	bool requires_idr();

private:
	PyroStreamConnectionServerInterface &server;
	PyroFling::RemoteAddress tcp_remote;
	PyroFling::RemoteAddress udp_remote;
	PyroFling::FileHandle timer_fd;
	pyro_progress_report progress = {};
	std::string remote_addr, remote_port;
	std::atomic<bool> needs_key_frame;

	uint64_t cookie;
	uint32_t packet_seq_video = 0;
	uint32_t packet_seq_audio = 0;
	pyro_kick_state_flags kick_flags = 0;

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

	uint16_t last_gamepad_seq = 0;
	bool kicked = false;
	bool valid_gamepad_seq = false;
	void write_packet(int64_t pts, int64_t dts, const void *data_, size_t size, bool is_audio, bool is_key_frame);
};

class PyroStreamServer final : public PyroStreamConnectionServerInterface
{
public:
	PyroStreamServer();
	void set_codec_parameters(const pyro_codec_parameters &codec_);
	pyro_codec_parameters get_codec_parameters() override;

	bool register_tcp_handler(PyroFling::Dispatcher &dispatcher, const PyroFling::FileHandle &,
	                          const PyroFling::RemoteAddress &remote,
	                          PyroFling::Handler *&handler);

	void write_video_packet(int64_t pts, int64_t dts, const void *data, size_t size, bool is_key_frame);
	void write_audio_packet(int64_t pts, int64_t dts, const void *data, size_t size);
	void handle_udp_datagram(PyroFling::Dispatcher &dispatcher, const PyroFling::RemoteAddress &remote,
	                         const void *msg, unsigned size);
	void release_connection(PyroStreamConnection *conn) override;
	bool should_force_idr();
	void set_phase_offset(int phase_offset_us) override;
	int get_phase_offset_us() const;
	void set_gamepad_state(const RemoteAddress &remote, const pyro_gamepad_state &state) override;
	const pyro_gamepad_state *get_updated_gamepad_state();

private:
	uint64_t cookie = 1000;
	std::mutex lock;
	std::vector<Util::IntrusivePtr<PyroStreamConnection>> connections;
	pyro_codec_parameters codec = {};
	unsigned idr_counter = 0;
	mutable std::atomic<int> phase_offset_us;

	// Current owner of virtual device. Super crude system, but hey :)
	RemoteAddress current_gamepad_remote;
	pyro_gamepad_state current_gamepad_state = {};
	bool new_gamepad_state = false;
};
}