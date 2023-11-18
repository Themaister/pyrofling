#include "pad_handler.hpp"

#ifdef HAVE_LINUX_INPUT
#include "input_linux.hpp"
#elif defined(HAVE_XINPUT_WINDOWS)
#include "xinput_windows.hpp"
#endif

namespace PyroFling
{
void PadHandler::dispatch(const Granite::JoypadStateEvent &e)
{
	using namespace Granite;

	pyro_gamepad_state state = {};
	if (e.is_connected(0))
	{
		auto &joy = e.get_state(0);
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
	}

	pyro->send_gamepad_state(state);
}

void gamepad_main_poll_loop(PyroStreamClient *client, std::atomic_bool *done)
{
	Granite::InputTracker tracker;
	PyroFling::PadHandler handler;
#ifdef HAVE_LINUX_INPUT
	Granite::LinuxInputManager input_manager;
	if (!input_manager.init(Granite::LINUX_INPUT_MANAGER_JOYPAD_BIT, &tracker))
	{
		LOGE("Failed to init input manager.\n");
		return;
	}

#elif defined(HAVE_XINPUT_WINDOWS)
	XInputManager input_manager;
	if (!input_manager.init(&tracker, nullptr))
	{
		LOGE("Failed to init input manager.\n");
		return;
	}
#endif

	handler.pyro = client;
	tracker.set_input_handler(&handler);

	// Be a bit aggressive about input polling.
	// Every ms matters when we're on critical network path.
	// Could try to be event-driven, but over UDP we'll have to resend often
	// anyway due to potential packet-loss. No need to be clever here.
#ifdef _WIN32
	timeBeginPeriod(1);
#endif

	// No need to be clever with condition variables since we wake up so frequently.
	while (!done || !*done)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
		if (!input_manager.poll())
		{
			LOGE("poll failed.\n");
			break;
		}

		tracker.dispatch_current_state(0.0);
	}

#ifdef _WIN32
	timeEndPeriod(1);
#endif
}
}