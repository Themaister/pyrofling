#define NOMINXMAX
#include "application.hpp"
#include "application_wsi_events.hpp"
#include "application_events.hpp"
#include "audio_mixer.hpp"
#include "global_managers_init.hpp"
#include "flat_renderer.hpp"
#include "filesystem.hpp"
#include "viewer_fonts.h"
#include "ui_manager.hpp"
#include "logging.hpp"
#include "slangmosh_blit.hpp"

using namespace Granite;

struct LatencyTestApplication final : Application, EventHandler
{
	LatencyTestApplication()
	{
		get_wsi().set_present_low_latency_mode(true);
		EVENT_MANAGER_REGISTER_LATCH(LatencyTestApplication, on_module_created, on_module_destroyed, Vulkan::DeviceShaderModuleReadyEvent);
		EVENT_MANAGER_REGISTER(LatencyTestApplication, on_joy_button, JoypadButtonEvent);
	}

	bool on_joy_button(const JoypadButtonEvent &e)
	{
		if (e.get_key() == JoypadKey::South && e.get_state() == JoypadKeyState::Pressed)
		{
			video.pressed = true;
		}
		else if (e.get_key() == JoypadKey::East && e.get_state() == JoypadKeyState::Pressed)
		{
			audio.pressed = true;
		}
		else if (e.get_key() == JoypadKey::Start && e.get_state() == JoypadKeyState::Pressed)
		{
			audio = {};
			video = {};
		}
		return true;
	}

	std::string get_name() override
	{
		return "pyrofling-latency-tester";
	}

	void on_module_created(const Vulkan::DeviceShaderModuleReadyEvent &e)
	{
		auto &device = e.get_device();
		Vulkan::ResourceLayout layout;
		Blit::Shaders<> blit_shaders(device, layout, 0);
	}

	void on_module_destroyed(const Vulkan::DeviceShaderModuleReadyEvent &)
	{
	}

	struct Mode
	{
		bool pressed = false;

		double offset = 0.0;
		double running_total = 0.0;
		int running_count = 0;
	} video, audio;

	struct SineStream : Granite::Audio::MixerStream
	{
		bool setup(float mixer_output_rate, unsigned mixer_channels, size_t) override
		{
			output_rate = mixer_output_rate;
			num_channels = mixer_channels;
			phase_iter = 2.0 * muglm::pi<double>() * 1600.0 / output_rate;
			return true;
		}

		size_t accumulate_samples(float *const *channels, const float *gain, size_t num_frames) noexcept override
		{
			for (size_t i = 0; i < num_frames; i++)
			{
				double ramp = muglm::min(1.0, phase * 0.1);
				if (phase > 200.0)
					ramp *= muglm::max(0.0, 1.0 + (200.0 - phase) / 200.0);

				for (unsigned c = 0; c < num_channels; c++)
					channels[c][i] += float(ramp * 0.20 * gain[c] * muglm::sin(phase));

				phase += phase_iter;
			}

			if (phase > 500.0f)
				return 0;
			return num_frames;
		}

		unsigned get_num_channels() const override
		{
			return num_channels;
		}

		float get_sample_rate() const override
		{
			return output_rate;
		}

		double phase_iter = 0.0;
		double phase = 0.0;
		float output_rate = 0.0f;
		unsigned num_channels = 0;
	};

	void render_frame(double frame_time, double elapsed_time) override
	{
		auto &device = get_wsi().get_device();

		auto cmd = device.request_command_buffer();
		auto rp_info = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
		rp_info.clear_color[0].float32[0] = 0.01f;
		rp_info.clear_color[0].float32[1] = 0.02f;
		rp_info.clear_color[0].float32[2] = 0.03f;
		cmd->begin_render_pass(rp_info);

		float width = cmd->get_viewport().width;
		float height = cmd->get_viewport().height;

		flat_renderer.begin();

		Mode *modes[] = { &video, &audio };
		for (auto *mode : modes)
		{
			if (mode->pressed)
			{
				mode->pressed = false;

				mode->offset = muglm::fract(elapsed_time);
				if (mode->offset > 0.5)
					mode->offset -= 1.0;

				mode->running_total += mode->offset;
				mode->running_count++;
			}
		}

		if (muglm::fract(frame_time + elapsed_time) < muglm::fract(elapsed_time))
			GRANITE_AUDIO_MIXER()->add_mixer_stream(new SineStream);

		float phase = muglm::fract(elapsed_time * 0.5);
		float sin_phase = muglm::sin(2.0f * muglm::pi<float>() * phase);

		float block_color = muglm::exp(-muglm::fract(elapsed_time) * 8.0);
		vec3 reference_color = vec3(0.0f, block_color, 0.0f);

		vec2 quad_offset = vec2(width, height) * vec2(0.5f + 0.3f * sin_phase, 0.5f);
		quad_offset -= 32.0f;
		flat_renderer.render_quad({ quad_offset, 0.0f }, { 64.0f, 64.0f }, vec4(vec3(block_color), 1.0f));

		quad_offset = vec2(width, height) * vec2(0.5f);
		quad_offset -= 32.0f;
		quad_offset.y -= 80.0f;

		flat_renderer.render_quad({ quad_offset, 0.0f }, { 64.0f, 64.0f }, vec4(reference_color, 1.0f));

		char text[256];

		if (video.running_count)
		{
			snprintf(text, sizeof(text), "Video || last offset = %8.3f ms, avg = %8.3f ms",
			         video.offset * 1e3, (video.running_total / video.running_count) * 1e3);
		}
		else
		{
			snprintf(text, sizeof(text), "Video || last offset = %8.3f ms", video.offset * 1e3);
		}

		flat_renderer.render_text(GRANITE_UI_MANAGER()->get_font(UI::FontSize::Large), text,
		                          { 100.0f, 100.0f, 0.0f }, { 400.0f, 100.0f },
		                          vec4(1.0f, 1.0f, 0.0f, 1.0f));

		if (audio.running_count)
		{
			snprintf(text, sizeof(text), "Audio || last offset = %8.3f ms, avg = %8.3f ms",
			         audio.offset * 1e3, (audio.running_total / audio.running_count) * 1e3);
		}
		else
		{
			snprintf(text, sizeof(text), "Audio || last offset = %8.3f ms", audio.offset * 1e3);
		}

		flat_renderer.render_text(GRANITE_UI_MANAGER()->get_font(UI::FontSize::Large), text,
		                          { 100.0f, 200.0f, 0.0f }, { 400.0f, 100.0f },
		                          vec4(1.0f, 1.0f, 0.0f, 1.0f));

		flat_renderer.flush(*cmd, {}, { width, height, 1 });

		cmd->end_render_pass();

		device.submit(cmd);
	}

	FlatRenderer flat_renderer;
};

namespace Granite
{
Application *application_create(int, char **)
{
	application_dummy();
	Global::init(Global::MANAGER_FEATURE_EVENT_BIT |
	             Global::MANAGER_FEATURE_AUDIO_MIXER_BIT |
	             Global::MANAGER_FEATURE_AUDIO_BACKEND_BIT |
	             Global::MANAGER_FEATURE_UI_MANAGER_BIT |
	             Global::MANAGER_FEATURE_ASSET_MANAGER_BIT |
	             Global::MANAGER_FEATURE_FILESYSTEM_BIT |
	             Global::MANAGER_FEATURE_THREAD_GROUP_BIT, 4);

	auto file = Util::make_handle<ConstantMemoryFile>(viewer_fonts, viewer_fonts_size);
	GRANITE_FILESYSTEM()->register_protocol("builtin", std::make_unique<BlobFilesystem>(std::move(file)));

	try
	{
		auto *app = new LatencyTestApplication();
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
} // namespace Granite

