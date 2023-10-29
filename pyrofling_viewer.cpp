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
#include "thread_group.hpp"
#include <cmath>

using namespace Granite;

struct VideoPlayerApplication : Application, EventHandler, DemuxerIOInterface
{
	explicit VideoPlayerApplication(const char *video_path,
	                                float video_buffer,
	                                float audio_buffer, float target, bool stats_)
		: stats(stats_)
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
			if (!pyro.handshake())
				throw std::runtime_error("Failed handshake.");

			decoder.set_io_interface(this);
			video_path = nullptr;
		}

		if (!decoder.init(GRANITE_AUDIO_MIXER(), video_path, opts))
			throw std::runtime_error("Failed to open file");

		float desired_audio_rate = -1.0f;
		if (!realtime) // We don't add a resampler ourselves, so aim for whatever sampling rate the file has.
			desired_audio_rate = decoder.get_audio_sample_rate();

		Global::init(Global::MANAGER_FEATURE_AUDIO_BACKEND_BIT, 0, desired_audio_rate);

		EVENT_MANAGER_REGISTER_LATCH(VideoPlayerApplication,
		                             on_module_created, on_module_destroyed,
		                             Vulkan::DeviceShaderModuleReadyEvent);

		if (target_realtime_delay <= 0.0)
			get_wsi().set_present_mode(Vulkan::PresentMode::UnlockedNoTearing);
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

		frame = std::move(next_frame);
		next_frame = {};
		need_acquire = true;
	}

	double last_done_ts = 0.0;
	double last_pts = 0.0;
	double audio_buffer[64] = {};
	unsigned audio_buffer_counter = 0;
	bool stats;

	void update_audio_buffer_stats()
	{
		audio_buffer[audio_buffer_counter++] = decoder.get_audio_buffering_duration();
		audio_buffer_counter %= 64;
		double avg = 0.0;
		for (auto &v: audio_buffer)
			avg += v;
		avg /= 64.0;
		LOGI("Buffered audio: %.3f ms, underflows %u.\n", avg * 1e3, decoder.get_audio_underflow_counter());
	}

	bool update(Vulkan::Device &device, double elapsed_time)
	{
		GRANITE_SCOPED_TIMELINE_EVENT("update");

		// Most aggressive method, not all that great for pacing ...
		if (realtime && target_realtime_delay <= 0.0)
		{
			bool had_acquire = false;
			// Always pick the most recent video frame we have.
			for (;;)
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
			if (!had_acquire && !decoder.acquire_video_frame(next_frame, 5000))
				return false;

			if (next_frame.view)
				shift_frame();

			// Audio syncs to video.
			// Dynamic rate control.
			decoder.latch_audio_buffering_target(0.030);

			if (stats)
			{
				// Measure frame jitter.
				if (frame.view)
				{
					if (last_done_ts != 0.0 && last_pts != 0.0)
					{
						double done_delta = frame.done_ts - last_done_ts;
						double pts_delta = frame.pts - last_pts;
						double jitter = done_delta - pts_delta;

						// We want these to be increasing at same rate. If there is variation,
						// we need to consider adding in extra delay to absorb the jitter.
						LOGI("Jitter: %.3f ms.\n", jitter * 1e3);
					}
					last_done_ts = frame.done_ts;
					last_pts = frame.pts;
				}

				if (stats)
					update_audio_buffer_stats();
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
				if (stats)
					update_audio_buffer_stats();
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

	void render_frame(double, double elapsed_time) override
	{
		auto &device = get_wsi().get_device();

		if (!update(device, elapsed_time))
			request_shutdown();

		auto cmd = device.request_command_buffer();
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
		cmd->end_render_pass();

		frame.sem.reset();
		device.submit(cmd, nullptr, 1, &frame.sem);
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
	double target_realtime_delay = 0.1;
};

static void print_help()
{
	LOGI("pyrofling-viewer [--video-buffer SECONDS] [--audio-buffer SECONDS] [--latency TARGET_LATENCY] [--stats]\n");
}

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	application_dummy();
	Global::init(Global::MANAGER_FEATURE_EVENT_BIT | Global::MANAGER_FEATURE_AUDIO_MIXER_BIT |
	             Global::MANAGER_FEATURE_THREAD_GROUP_BIT, 4);

	float video_buffer = 0.5f;
	float audio_buffer = 0.5f;
	float target_delay = 0.1f;
	const char *path = nullptr;
	bool stats = false;

	Util::CLICallbacks cbs;
	cbs.add("--help", [&](Util::CLIParser &parser) { parser.end(); });
	cbs.add("--video-buffer", [&](Util::CLIParser &parser) { video_buffer = float(parser.next_double()); });
	cbs.add("--audio-buffer", [&](Util::CLIParser &parser) { audio_buffer = float(parser.next_double()); });
	cbs.add("--latency", [&](Util::CLIParser &parser) { target_delay = float(parser.next_double()); });
	cbs.add("--stats", [&](Util::CLIParser &) { stats = true; });
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
		auto *app = new VideoPlayerApplication(path, video_buffer, audio_buffer, target_delay, stats);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite

