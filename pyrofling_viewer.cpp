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

using namespace Granite;

struct VideoPlayerApplication : Application, EventHandler
{
	explicit VideoPlayerApplication(const char *video_path)
	{
		VideoDecoder::DecodeOptions opts;
		// Crude :)
		opts.realtime = strstr(video_path, "://") != nullptr;
		realtime = opts.realtime;
		if (!decoder.init(GRANITE_AUDIO_MIXER(), video_path, opts))
			throw std::runtime_error("Failed to open file");

		EVENT_MANAGER_REGISTER_LATCH(VideoPlayerApplication,
		                             on_module_created, on_module_destroyed,
		                             Vulkan::DeviceShaderModuleReadyEvent);

		if (realtime)
			get_wsi().set_present_mode(Vulkan::PresentMode::UnlockedMaybeTear);
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

	bool update(Vulkan::Device &device, double elapsed_time)
	{
		if (realtime)
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
			if (!had_acquire && !decoder.acquire_video_frame(next_frame))
				return false;

			if (next_frame.view)
				shift_frame();

			// Audio syncs to video.
			// Dynamic rate control.

			// Give 20ms extra audio buffering for good measure.
			decoder.latch_audio_presentation_target(frame.pts - 0.02);
		}
		else
		{
			// Synchronize based on audio. Prioritize smoothness over latency.

			// Based on the audio PTS, we want to display a video frame that is slightly larger.
			double target_pts = decoder.get_estimated_audio_playback_timestamp(elapsed_time);
			if (target_pts < 0.0)
				target_pts = elapsed_time;

			// Update the latest frame. We want the closest PTS to target_pts.
			if (!next_frame.view)
				if (decoder.try_acquire_video_frame(next_frame) < 0 && target_pts > frame.pts)
					return false;

			LOGI("Video buffer latency: %.3f, audio buffer latency: %.3f s\n",
			     decoder.get_last_video_buffering_pts() - target_pts,
			     decoder.get_audio_buffering_duration());

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
			cmd->draw(3);
		}
		cmd->end_render_pass();

		frame.sem.reset();
		device.submit(cmd, nullptr, 1, &frame.sem);
	}

	VideoDecoder decoder;
	VideoFrame frame, next_frame;
	bool need_acquire = false;
	Vulkan::Program *blit = nullptr;
	bool realtime = false;
};

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	GRANITE_APPLICATION_SETUP_FILESYSTEM();

	if (argc != 2)
		return nullptr;

	try
	{
		auto *app = new VideoPlayerApplication(argv[1]);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite

