/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "ffmpeg_decode.hpp"
#include "application.hpp"
#include "application_wsi_events.hpp"
#include "audio_mixer.hpp"
#include "slangmosh_decode_iface.hpp"
#include "slangmosh_decode.hpp"
#include "slangmosh_blit.hpp"
#include "global_managers_init.hpp"
#include "pyro_client.hpp"
#include "cli_parser.hpp"
#include "string_helpers.hpp"
#include "timeline_trace_file.hpp"
#include "pyro_protocol.h"
#include "flat_renderer.hpp"
#include "filesystem.hpp"
#include "viewer_fonts.h"
#include "ui_manager.hpp"
#include "pad_handler.hpp"
#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

using namespace Granite;

template <typename T, size_t N, typename U>
static void push_sliding_window(T (&v)[N], U value)
{
	memmove(&v[0], &v[1], sizeof(v) - sizeof(v[0]));
	v[N - 1] = T(value);
}

struct VideoPlayerApplication final : Application, EventHandler, DemuxerIOInterface
{
	VideoPlayerApplication(const char *video_path,
	                       float video_buffer,
	                       float audio_buffer, float target,
	                       double phase_locked_offset_, bool phase_locked_enable_,
	                       double deadline_, bool deadline_enable_, const char *hwdevice)
		: phase_locked_offset(phase_locked_offset_), phase_locked_enable(phase_locked_enable_)
		, deadline(deadline_), deadline_enable(deadline_enable_)
	{
		// Debug
		//PyroFling::PyroStreamClient::set_simulate_reordering(true);
		//PyroFling::PyroStreamClient::set_simulate_drop(true);
		////

		get_wsi().set_low_latency_mode(true);

		VideoDecoder::DecodeOptions opts;
		// Crude :)
		opts.realtime = strstr(video_path, "://") != nullptr;
		opts.target_realtime_audio_buffer_time = audio_buffer;
		opts.target_video_buffer_time = video_buffer;
		opts.blocking = true;
		opts.hwdevice = hwdevice;
		realtime = opts.realtime;
		target_realtime_delay = target;

		if (strncmp(video_path, "pyro://", 7) == 0)
		{
			auto split = Util::split(video_path + 7, ":");
			if (split.size() != 2)
				throw std::runtime_error("Must specify both IP and port.");
			LOGI("Connecting to raw pyrofling %s:%s.\n",
			     split[0].c_str(), split[1].c_str());

			if (!pyro.connect(split[0].c_str(), split[1].c_str()))
				throw std::runtime_error("Failed to connect to server.");
			if (!pyro.handshake(PYRO_KICK_STATE_VIDEO_BIT | PYRO_KICK_STATE_AUDIO_BIT | PYRO_KICK_STATE_GAMEPAD_BIT))
				throw std::runtime_error("Failed handshake.");

			decoder.set_io_interface(this);
			video_path = nullptr;

			poll_thread_dead = false;
			poll_thread = std::thread([this]() {
				PyroFling::gamepad_main_poll_loop(&pyro, &poll_thread_dead);
			});

			if (opts.target_realtime_audio_buffer_time > 0.10f && (phase_locked_enable || target_realtime_delay <= 0.0))
				opts.target_realtime_audio_buffer_time = 0.10f;
		}
		else
			phase_locked_enable = false;

		if (!decoder.init(GRANITE_AUDIO_MIXER(), video_path, opts))
			throw std::runtime_error("Failed to open file");

		float desired_audio_rate = -1.0f;
		if (!realtime) // We don't add a resampler ourselves, so aim for whatever sampling rate the file has.
			desired_audio_rate = decoder.get_audio_sample_rate();

		Global::init(Global::MANAGER_FEATURE_AUDIO_BACKEND_BIT, 0, desired_audio_rate);

		EVENT_MANAGER_REGISTER_LATCH(VideoPlayerApplication,
		                             on_module_created, on_module_destroyed,
		                             Vulkan::DeviceShaderModuleReadyEvent);

		EVENT_MANAGER_REGISTER(VideoPlayerApplication, on_key_pressed, KeyboardEvent);

		if (target_realtime_delay <= 0.0 && !phase_locked_enable)
			get_wsi().set_present_mode(Vulkan::PresentMode::UnlockedNoTearing);

#ifdef _WIN32
		if (deadline_enable)
			timeBeginPeriod(1);
#endif
	}

	bool on_key_pressed(const KeyboardEvent &e)
	{
		if (e.get_key() == Key::V && e.get_key_state() == KeyState::Pressed)
			stats.enable = !stats.enable;
		return true;
	}

	bool enable_joypad_input_manager() override
	{
		return false;
	}

	~VideoPlayerApplication() override
	{
		if (poll_thread.joinable())
		{
			// Poll thread wakes up constantly, so no need to be cute and use condition variables.
			poll_thread_dead = true;
			poll_thread.join();
		}

#ifdef _WIN32
		if (deadline_enable)
			timeEndPeriod(1);
#endif
	}

	std::string get_name() override
	{
		return "pyrofling-viewer";
	}

	unsigned get_default_width() override
	{
		return decoder.get_width();
	}

	unsigned get_default_height() override
	{
		return decoder.get_height();
	}

	void shift_frame()
	{
		if (frame.view)
		{
			// If we never actually read the image and discarded it,
			// we just forward the acquire semaphore directly to release.
			// This resolves any write-after-write hazard for the image.
			VK_ASSERT(frame.sem);
			decoder.release_video_frame(frame.index, std::move(frame.sem));
		}

		if (frame.view && next_frame.view)
			push_sliding_window(stats.server_frame_time, next_frame.pts - frame.pts);

		frame = std::move(next_frame);
		next_frame = {};
		need_acquire = true;
	}

	double last_done_ts = 0.0;
	double last_pts = 0.0;
	double phase_locked_offset;
	bool phase_locked_enable;
	double deadline;
	bool deadline_enable;
	unsigned long long missed_deadlines = 0;
	std::thread poll_thread;
	std::atomic_bool poll_thread_dead;

	struct
	{
		float pts_deltas[150];
		float phase_offsets[150];
		float audio_delay_buffer[150];
		float local_frame_time[150];
		float server_frame_time[150];
		float ping[150];
		float buffered_video[150];
		bool enable = false;
	} stats = {};

	void update_audio_buffer_stats()
	{
		push_sliding_window(stats.audio_delay_buffer, decoder.get_audio_buffering_duration());
	}

	bool is_running_pyro() const
	{
		return poll_thread.joinable();
	}

	bool update(Vulkan::Device &device, double frame_time, double elapsed_time)
	{
		GRANITE_SCOPED_TIMELINE_EVENT("update");

		push_sliding_window(stats.local_frame_time, frame_time);
		update_audio_buffer_stats();

		if (is_running_pyro())
			push_sliding_window(stats.ping, pyro.get_current_ping_delay());

		// Most aggressive method, not all that great for pacing ...
		if (realtime && (target_realtime_delay <= 0.0 || phase_locked_enable))
		{
			double target_done = double(Util::get_current_time_nsecs()) * 1e-9 + phase_locked_offset;
			bool had_acquire = false;
			unsigned target_frames = phase_locked_enable ? 3 : 0;

			// Catch up and then rely on phase locked loop to tune latency.
			while (decoder.get_num_ready_video_frames() > target_frames)
			{
				if (next_frame.view)
					shift_frame();
				int ret = decoder.try_acquire_video_frame(next_frame);

				if (ret < 0)
					return false;
				else if (ret == 0)
					break;
				else
					had_acquire = true;
			}

			// Block until we have received at least one new frame.
			// No point duplicating presents.

			if (!had_acquire)
			{
				if (deadline_enable)
				{
					// In deadline mode, we want to keep the pace going, even when there are drops server side.
					// Also aims to avoid bad tearing which will inevitably happen.
					// When deadlines are close, we still want FIFO_RELAXED however just in case we barely miss vblank.
					if (!decoder.acquire_video_frame(next_frame, int(deadline * 1e3)))
					{
						if (decoder.is_eof())
							return false;
						missed_deadlines++;
					}
				}
				else if (!decoder.acquire_video_frame(next_frame, 5000))
					return false;
			}

			if (next_frame.view)
				shift_frame();

			if (phase_locked_enable && frame.view)
			{
				double phase_offset = target_done - double(frame.done_ts) * 1e-9;
				memmove(stats.phase_offsets, stats.phase_offsets + 1,
				        sizeof(stats.phase_offsets) - sizeof(stats.phase_offsets[0]));
				stats.phase_offsets[sizeof(stats.phase_offsets) / sizeof(stats.phase_offsets[0]) - 1] = float(phase_offset);
				push_sliding_window(stats.phase_offsets, phase_offset);

				int target_phase_offset_us = int(phase_offset * 1e6);
				if (!pyro.send_target_phase_offset(target_phase_offset_us))
					LOGE("Failed to send phase offset.\n");
			}

			// Audio syncs to video.
			// Dynamic rate control.
			decoder.latch_audio_buffering_target(0.030);

			// Measure frame jitter. Ideally, the time delta in decode done time (client side) should
			// equal the time delta in PTS domain (server side).
			if (frame.view)
			{
				double done_ts = double(frame.done_ts) * 1e-9;
				if (last_done_ts != 0.0 && last_pts != 0.0)
				{
					double done_delta = done_ts - last_done_ts;
					double pts_delta = frame.pts - last_pts;
					double jitter = done_delta - pts_delta;
					push_sliding_window(stats.pts_deltas, jitter);
				}
				last_done_ts = done_ts;
				last_pts = frame.pts;
			}
		}
		else
		{
			// Synchronize based on audio. Prioritize smoothness over latency.

			double target_pts;

			if (realtime)
			{
				// Based on the video PTS.
				// Aim for 100ms of buffering to absorb network jank.
				target_pts = decoder.latch_estimated_video_playback_timestamp(elapsed_time, target_realtime_delay);
			}
			else
			{
				// Based on the audio PTS, we want to display a video frame that is slightly larger.
				target_pts = decoder.get_estimated_audio_playback_timestamp(elapsed_time);
			}

			if (target_pts < 0.0)
				target_pts = elapsed_time;

			// Update the latest frame. We want the closest PTS to target_pts.
			if (!next_frame.view)
			{
				if (decoder.try_acquire_video_frame(next_frame) < 0 && target_pts > frame.pts)
					return false;
			}
			else if (decoder.is_eof())
				return false;

			while (next_frame.view)
			{
				// If we have two candidates, shift out frame if next_frame PTS is closer.
				double d_current = std::abs(frame.pts - target_pts);
				double d_next = std::abs(next_frame.pts - target_pts);

				// In case we get two frames with same PTS for whatever reason, ensure forward progress.
				// The less-equal check is load-bearing.
				if (d_next <= d_current || !frame.view)
				{
					shift_frame();

					// Try to catch up quickly by skipping frames if we have to.
					// Defer any EOF handling to next frame.
					decoder.try_acquire_video_frame(next_frame);
				}
				else
					break;
			}
		}

		push_sliding_window(stats.buffered_video, decoder.get_last_video_buffering_pts() - frame.pts);

		if (need_acquire)
		{
			// When we have committed to display this video frame,
			// inject the wait semaphore.
			device.add_wait_semaphore(
					Vulkan::CommandBuffer::Type::Generic, std::move(frame.sem),
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, true);
			frame.sem = {};
			need_acquire = false;
		}

		return true;
	}

	void begin(Vulkan::Device &device)
	{
		Vulkan::ResourceLayout layout;
		FFmpegDecode::Shaders<> shaders(device, layout, 0);

		if (!decoder.begin_device_context(&device, shaders))
			LOGE("Failed to begin device context.\n");
		if (!decoder.play())
			LOGE("Failed to begin playback.\n");

		Blit::Shaders<> blit_shaders(device, layout, 0);
		blit = device.request_program(blit_shaders.quad, blit_shaders.blit);
	}

	void end()
	{
		frame = {};
		next_frame = {};
		decoder.stop();
		decoder.end_device_context();
	}

	void on_module_created(const Vulkan::DeviceShaderModuleReadyEvent &e)
	{
		begin(e.get_device());
	}

	void on_module_destroyed(const Vulkan::DeviceShaderModuleReadyEvent &)
	{
		end();
	}

	void render_frame(double frame_time, double elapsed_time) override
	{
		auto &device = get_wsi().get_device();

		if (!update(device, frame_time, elapsed_time))
			request_shutdown();

		auto cmd = device.request_command_buffer();

		{
			GRANITE_SCOPED_TIMELINE_EVENT("build-cmd");
			auto rp = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);

			cmd->begin_render_pass(rp);
			if (frame.view)
			{
				cmd->set_opaque_sprite_state();
				cmd->set_program(blit);
				cmd->set_texture(0, 0, *frame.view, Vulkan::StockSampler::LinearClamp);

				auto vp = cmd->get_viewport();
				float video_aspect = float(decoder.get_width()) / float(decoder.get_height());
				float vp_aspect = vp.width / vp.height;

				if (vp_aspect > video_aspect)
				{
					float target_width = vp.height * video_aspect;
					vp.x = std::round(0.5f * (vp.width - target_width));
					vp.width = std::round(target_width);
				}
				else if (vp_aspect < video_aspect)
				{
					float target_height = vp.width / video_aspect;
					vp.y = std::round(0.5f * (vp.height - target_height));
					vp.height = std::round(target_height);
				}

				cmd->set_viewport(vp);
				cmd->draw(3);
			}

			if (stats.enable)
			{
				flat_renderer.begin();

				float y_offset = 15.0f;
				render_sliding_window("Server pace", 15.0f, y_offset, 300, 100, stats.server_frame_time);
				render_sliding_window("Client pace", 15.0f + 320.0f, y_offset, 300, 100, stats.local_frame_time);
				y_offset += 110.0f;

				if (phase_locked_enable)
				{
					render_sliding_window("Phase offset", 15.0f, y_offset, 300, 100, stats.phase_offsets, true);
					render_sliding_window("Jitter", 15.0f + 320.0f, y_offset, 300, 100, stats.pts_deltas);
					y_offset += 110.0f;
				}

				render_sliding_window("Audio buffer", 15.0f, y_offset, 300, 100, stats.audio_delay_buffer);
				render_sliding_window("Video buffer", 15.0f + 320.0f, y_offset, 300, 100, stats.buffered_video);
				y_offset += 110.0f;
				render_sliding_window("Ping", 15.0f, y_offset, 300, 100, stats.ping);

				if (deadline_enable)
				{
					flat_renderer.render_quad({15.0f + 320.0f, y_offset, 0.5f}, {300, 45}, {0.0f, 0.0f, 0.0f, 0.5f});
					char text[128];
					snprintf(text, sizeof(text), "Missed deadline: %llu\n", missed_deadlines);
					flat_renderer.render_text(GRANITE_UI_MANAGER()->get_font(Granite::UI::FontSize::Large), text,
					                          { 15.0f + 320.0f + 10.0f, y_offset + 10.0f, 0 },
					                          { 300.0f - 10.0f, 45.0f - 10.0f});
				}

				flat_renderer.flush(*cmd, {}, {cmd->get_viewport().width, cmd->get_viewport().height, 1.0f});
			}

			cmd->end_render_pass();
		}

		{
			GRANITE_SCOPED_TIMELINE_EVENT("submit");
			frame.sem.reset();
			device.submit(cmd, nullptr, 1, &frame.sem);
		}
	}

	template <size_t N>
	void render_sliding_window(const char *tag, float x, float y, float width, float height, const float (&ts)[N],
	                           bool is_signed = false)
	{
		flat_renderer.render_quad({x, y, 0.5f}, {width, height}, {0.0f, 0.0f, 0.0f, 0.5f});
		flat_renderer.render_quad({x, y + 45.0f, 0.4f}, {width, height - 45.0f}, {0.0f, 0.0f, 0.0f, 0.5f});

		vec2 offsets[N];
		float avg = 0.0f;
		for (size_t i = 0; i < N; i++)
		{
			offsets[i].x = x + width * float(i) / float(N - 1);
			float normalized_time = 60.0f * abs(ts[i]);

			if (is_signed)
			{
				normalized_time = clamp(normalized_time, -1.0f, 1.0f);
				offsets[i].y = y + 45.0f + (height - 45.0f) * (0.5f - 0.5f * normalized_time);
			}
			else
			{
				normalized_time = clamp(normalized_time, 0.0f, 2.0f);
				offsets[i].y = y + 45.0f + (height - 45.0f) * (1.0f - 0.5f * normalized_time);
			}

			if (is_signed)
				avg += ts[i];
			else
				avg += abs(ts[i]);
		}

		avg /= float(N);

		char text[128];
		snprintf(text, sizeof(text), "%s: %.3f ms\n", tag, 1e3 * avg);
		flat_renderer.render_text(GRANITE_UI_MANAGER()->get_font(Granite::UI::FontSize::Large), text,
		                          { x + 10.0f, y + 10.0f, 0 }, { width - 10.0f, height - 10.0f});

		flat_renderer.render_line_strip(offsets, 0.0f, N, vec4(1.0f));

		offsets[0].x = x;
		offsets[0].y = y + 45.0f + (height - 45.0f) * 0.5f;
		offsets[1].x = x + width;
		offsets[1].y = y + 45.0f + (height - 45.0f) * 0.5f;
		flat_renderer.render_line_strip(offsets, 0.1f, 2, vec4(0.0f, 1.0f, 0.0f, 0.2f));
	}

	pyro_codec_parameters get_codec_parameters() override
	{
		return pyro.get_codec_parameters();
	}

	bool wait_next_packet() override
	{
		return pyro.wait_next_packet();
	}

	const void *get_data() override
	{
		return pyro.get_packet_data();
	}

	size_t get_size() override
	{
		return pyro.get_packet_size();
	}

	pyro_payload_header get_payload_header() override
	{
		return pyro.get_payload_header();
	}

	PyroFling::PyroStreamClient pyro;
	VideoDecoder decoder;
	VideoFrame frame, next_frame;
	bool need_acquire = false;
	Vulkan::Program *blit = nullptr;
	bool realtime = false;
	double target_realtime_delay = 0.0;
	FlatRenderer flat_renderer;
};

static void print_help()
{
	LOGI("pyrofling-viewer [--video-buffer SECONDS] [--audio-buffer SECONDS] "
	     "[--latency TARGET_LATENCY] [--phase-locked OFFSET_SECONDS] [--deadline SECONDS] [--hwdevice TYPE]\n");
}

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	application_dummy();
	Global::init(Global::MANAGER_FEATURE_EVENT_BIT | Global::MANAGER_FEATURE_AUDIO_MIXER_BIT |
	             Global::MANAGER_FEATURE_UI_MANAGER_BIT |
	             Global::MANAGER_FEATURE_ASSET_MANAGER_BIT |
	             Global::MANAGER_FEATURE_FILESYSTEM_BIT |
	             Global::MANAGER_FEATURE_THREAD_GROUP_BIT, 4);

	auto file = Util::make_handle<ConstantMemoryFile>(viewer_fonts, viewer_fonts_size);
	GRANITE_FILESYSTEM()->register_protocol("builtin", std::make_unique<BlobFilesystem>(std::move(file)));

	float video_buffer = 0.5f;
	float audio_buffer = 0.5f;
	float target_delay = 0.1f;
	const char *path = nullptr;
	double phase_locked_offset = 0.0;
	bool phase_locked_enable = false;
	double deadline = 0.0;
	bool deadline_enable = false;
	const char *hwdevice = nullptr;

	Util::CLICallbacks cbs;
	cbs.add("--help", [&](Util::CLIParser &parser) { parser.end(); });
	cbs.add("--video-buffer", [&](Util::CLIParser &parser) { video_buffer = float(parser.next_double()); });
	cbs.add("--audio-buffer", [&](Util::CLIParser &parser) { audio_buffer = float(parser.next_double()); });
	cbs.add("--latency", [&](Util::CLIParser &parser) { target_delay = float(parser.next_double()); });
	cbs.add("--phase-locked", [&](Util::CLIParser &parser) { phase_locked_offset = parser.next_double(); phase_locked_enable = true; });
	cbs.add("--deadline", [&](Util::CLIParser &parser) { deadline = parser.next_double(); deadline_enable = true; });
	cbs.add("--hwdevice", [&](Util::CLIParser &parser) { hwdevice = parser.next_string(); });
	cbs.default_handler = [&](const char *path_) { path = path_; };
	Util::CLIParser parser(std::move(cbs), argc - 1, argv + 1);

	if (!parser.parse())
	{
		print_help();
		return nullptr;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		exit(EXIT_SUCCESS);
	}
	else if (!path)
	{
		LOGI("Path required.\n");
		print_help();
		return nullptr;
	}

	if (video_buffer < target_delay * 2.0f)
	{
		LOGW("Video buffer (%.3f) is less than twice the target delay (%.3f), expect jank!\n",
			 video_buffer, target_delay);
	}

	if (audio_buffer < target_delay * 2.0f)
	{
		LOGW("Audio buffer (%.3f) is less than twice the target delay (%.3f), expect jank!\n",
		     audio_buffer, target_delay);
	}

	try
	{
		auto *app = new VideoPlayerApplication(path, video_buffer, audio_buffer, target_delay,
		                                       phase_locked_offset, phase_locked_enable,
		                                       deadline, deadline_enable, hwdevice);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite

