#include "cli_parser.hpp"
#include "string_helpers.hpp"
#include "pyro_client.hpp"
#include <SDL3/SDL.h>
#include "input_sdl.hpp"
#include "virtual_gamepad.hpp"
#include <thread>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

using namespace Granite;

static void print_help()
{
	LOGI("pyrofling-gamepad ip:port\n");
}

static void poll_thread_main(PyroFling::PyroStreamClient &pyro, InputTracker &tracker)
{
	struct PadHandler : Granite::InputTrackerHandler
	{
		PyroFling::PyroStreamClient *pyro = nullptr;
		void dispatch(const Granite::TouchDownEvent &) override {}
		void dispatch(const Granite::TouchUpEvent &) override {}
		void dispatch(const Granite::TouchGestureEvent &) override {}
		void dispatch(const Granite::JoypadButtonEvent &) override {}
		void dispatch(const Granite::JoypadAxisEvent &) override {}
		void dispatch(const Granite::KeyboardEvent &) override {}
		void dispatch(const Granite::OrientationEvent &) override {}
		void dispatch(const Granite::MouseButtonEvent &) override {}
		void dispatch(const Granite::MouseMoveEvent &) override {}
		void dispatch(const Granite::InputStateEvent &) override {}
		void dispatch(const Granite::JoypadConnectionEvent &) override {}
		void dispatch(const Granite::JoypadStateEvent &e) override
		{
			using namespace Granite;

			pyro_gamepad_state state = {};
			bool done = false;

			for (unsigned i = 0, n = e.get_num_indices(); i < n && !done; i++)
			{
				if (!e.is_connected(i))
					continue;

				auto &joy = e.get_state(i);

				// Don't cause feedbacks when used locally.
				if (joy.vid == PyroFling::VirtualGamepad::FAKE_VID &&
				    joy.pid == PyroFling::VirtualGamepad::FAKE_PID)
					continue;

				state.axis_lx = int16_t(0x7fff * joy.axis[int(JoypadAxis::LeftX)]);
				state.axis_ly = int16_t(0x7fff * joy.axis[int(JoypadAxis::LeftY)]);
				state.axis_rx = int16_t(0x7fff * joy.axis[int(JoypadAxis::RightX)]);
				state.axis_ry = int16_t(0x7fff * joy.axis[int(JoypadAxis::RightY)]);
				state.hat_x += (joy.button_mask & (1 << int(JoypadKey::Left))) != 0 ? -1 : 0;
				state.hat_x += (joy.button_mask & (1 << int(JoypadKey::Right))) != 0 ? +1 : 0;
				state.hat_y += (joy.button_mask & (1 << int(JoypadKey::Up))) != 0 ? -1 : 0;
				state.hat_y += (joy.button_mask & (1 << int(JoypadKey::Down))) != 0 ? +1 : 0;
				state.lz = uint8_t(255.0f * joy.axis[int(JoypadAxis::LeftTrigger)]);
				state.rz = uint8_t(255.0f * joy.axis[int(JoypadAxis::RightTrigger)]);
				if (joy.button_mask & (1 << int(JoypadKey::East)))
					state.buttons |= PYRO_PAD_EAST_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::South)))
					state.buttons |= PYRO_PAD_SOUTH_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::West)))
					state.buttons |= PYRO_PAD_WEST_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::North)))
					state.buttons |= PYRO_PAD_NORTH_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::LeftShoulder)))
					state.buttons |= PYRO_PAD_TL_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::RightShoulder)))
					state.buttons |= PYRO_PAD_TR_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::LeftThumb)))
					state.buttons |= PYRO_PAD_THUMBL_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::RightThumb)))
					state.buttons |= PYRO_PAD_THUMBR_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::Start)))
					state.buttons |= PYRO_PAD_START_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::Select)))
					state.buttons |= PYRO_PAD_SELECT_BIT;
				if (joy.button_mask & (1 << int(JoypadKey::Mode)))
					state.buttons |= PYRO_PAD_MODE_BIT;

				LOGI("Dispatching axis: %d, %d\n", state.axis_lx, state.axis_ly);

				done = true;
			}

			if (!pyro->send_gamepad_state(state))
				dead = true;
		}

		bool dead = false;
	};

	PadHandler handler;
	handler.pyro = &pyro;

#ifdef _WIN32
	timeBeginPeriod(1);
#endif

	while (!handler.dead)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		std::lock_guard<std::mutex> holder{tracker.get_lock()};
		tracker.dispatch_current_state(0.0, &handler);
	}

	SDL_Event event = {};
	event.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&event);

#ifdef _WIN32
	timeEndPeriod(1);
#endif
}

int main(int argc, char **argv)
{
	Util::CLICallbacks cbs;
	std::string addr;
	cbs.add("--help", [&](Util::CLIParser &parser) { parser.end(); });
	cbs.default_handler = [&](const char *addr_) { addr = addr_; };
	Util::CLIParser parser(std::move(cbs), argc - 1, argv + 1);

	if (!parser.parse())
	{
		print_help();
		return EXIT_FAILURE;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}
	else if (addr.empty())
	{
		LOGI("Path required.\n");
		print_help();
		return EXIT_FAILURE;
	}

	auto split = Util::split(addr, ":");
	if (split.size() != 2)
	{
		LOGE("Must specify both IP and port.\n");
		return EXIT_FAILURE;
	}

	LOGI("Connecting to raw pyrofling %s:%s.\n",
	     split[0].c_str(), split[1].c_str());

	PyroFling::PyroStreamClient pyro;

	if (!pyro.connect(split[0].c_str(), split[1].c_str()))
	{
		LOGE("Failed to connect to server.\n");
		return EXIT_FAILURE;
	}

	if (!pyro.handshake(PYRO_KICK_STATE_GAMEPAD_BIT))
	{
		LOGE("Failed handshake.\n");
		return EXIT_FAILURE;
	}

	InputTracker tracker;
	InputTrackerSDL pad;

	if (SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS) < 0)
	{
		LOGE("Failed to init SDL.\n");
		return EXIT_FAILURE;
	}

	const auto dispatcher = [&tracker](std::function<void ()> func) {
		std::lock_guard<std::mutex> holder{tracker.get_lock()};
		func();
	};

	if (!pad.init(tracker, dispatcher))
		return EXIT_FAILURE;

	std::thread thr{[&pyro, &tracker]() {
		poll_thread_main(pyro, tracker);
	}};

	// Should keep going until killed.
	SDL_Event e;
	while (SDL_WaitEvent(&e))
	{
		if (e.type == SDL_EVENT_QUIT)
			break;
		pad.process_sdl_event(e, tracker, dispatcher);
	}

	thr.join();
}
