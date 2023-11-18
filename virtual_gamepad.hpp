#pragma once

#include <stdint.h>
#include "pyro_protocol.h"

namespace PyroFling
{
class VirtualGamepad
{
public:
	VirtualGamepad();
	~VirtualGamepad();
	void operator=(const VirtualGamepad &) = delete;
	VirtualGamepad(const VirtualGamepad &) = delete;

	void report_state(const pyro_gamepad_state &state);

	enum { FAKE_VID = 0x8998, FAKE_PID = 0xffee };

private:
	int uinput_fd = -1;
	pyro_gamepad_state last_state = {};
};
}