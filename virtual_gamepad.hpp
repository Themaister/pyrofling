#pragma once

#include <stdint.h>
#include "pyro_protocol.h"

namespace PyroFling
{
class VirtualGamepad
{
public:
	enum class DebugMode { None, RightAnalogToMouse };
	VirtualGamepad(DebugMode mode, uint32_t fake_vid = 0, uint32_t fake_pid = 0, const char *fake_name = nullptr);
	~VirtualGamepad();
	void operator=(const VirtualGamepad &) = delete;
	VirtualGamepad(const VirtualGamepad &) = delete;

	void report_state(const pyro_gamepad_state &state);

	enum { FAKE_VID = 0x8998, FAKE_PID = 0xffee, FAKE_PID_MOUSE = FAKE_PID + 3 };

private:
	DebugMode debug_mode = DebugMode::None;
	int uinput_fd = -1;
	pyro_gamepad_state last_state = {};

	VirtualGamepad();

	void init_gamepad(uint32_t fake_vid, uint32_t fake_pid, const char *fake_name);
	void init_debug_mouse();
};
}