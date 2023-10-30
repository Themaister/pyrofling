#include "virtual_gamepad.hpp"
#include "pyro_protocol.h"
#include "bitops.hpp"
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <string.h>

namespace PyroFling
{
static const int button_mapping[] =
{
	BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
	BTN_TL, BTN_TR,
	BTN_THUMBL, BTN_THUMBR,
	BTN_START, BTN_SELECT,
	BTN_MODE,
};

VirtualGamepad::VirtualGamepad()
{
	uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (uinput_fd < 0)
		throw std::runtime_error("Failed to open /dev/uinput.");

	if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0)
	{
		close(uinput_fd);
		throw std::runtime_error("Failed to set EV_KEY.");
	}

	if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0)
	{
		close(uinput_fd);
		throw std::runtime_error("Failed to set EV_KEY.");
	}

	// Emulate a generic pad according that conforms to Linux evdev specs and basically emulates a PS4 controller.

	for (auto btn : button_mapping)
	{
		if (ioctl(uinput_fd, UI_SET_KEYBIT, btn) < 0)
		{
			close(uinput_fd);
			throw std::runtime_error("Failed to set keybit.");
		}
	}

	static const int axes[] =
	{
		ABS_X, ABS_Y, ABS_RX, ABS_RY,
		ABS_Z, ABS_RZ,
		ABS_HAT0X, ABS_HAT0Y,
	};

	for (auto axis : axes)
	{
		if (ioctl(uinput_fd, UI_SET_ABSBIT, axis) < 0)
		{
			close(uinput_fd);
			throw std::runtime_error("Failed to set absbit.");
		}

		int range;
		if (axis == ABS_HAT0X || axis == ABS_HAT0Y)
			range = 1;
		else if (axis == ABS_Z || axis == ABS_RZ)
			range = 0xff;
		else
			range = 0x7fff;

		struct uinput_abs_setup abs_setup = {};
		abs_setup.code = axis;
		abs_setup.absinfo.minimum = (axis == ABS_Z || axis == ABS_RZ) ? 0 : -range;
		abs_setup.absinfo.maximum = range;

		if (ioctl(uinput_fd, UI_ABS_SETUP, &abs_setup) < 0)
		{
			close(uinput_fd);
			throw std::runtime_error("Failed to set absbit.");
		}
	}

	uinput_setup usetup = {};
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x8998; // Dummy values.
	usetup.id.product = 0xffee;
	strcpy(usetup.name, "PyroFling virtual gamepad");

	if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0 || ioctl(uinput_fd, UI_DEV_CREATE) < 0)
	{
		close(uinput_fd);
		throw std::runtime_error("Failed to setup uinput device.");
	}
}

static void write_event(int fd, int type, int code, int val)
{
	struct input_event e = {};
	e.code = code;
	e.type = type;
	e.value = val;
	if (write(fd, &e, sizeof(e)) < 0)
		fprintf(stderr, "Failed to write uinput event.\n");
}

void VirtualGamepad::report_state(const pyro_gamepad_state &state)
{
	uint16_t delta_btn = state.buttons ^ last_state.buttons;

	Util::for_each_bit(delta_btn, [this, v = state.buttons](unsigned bit) {
		if (bit < sizeof(button_mapping) / sizeof(button_mapping[0]))
			write_event(uinput_fd, EV_KEY, button_mapping[bit], (v & (1u << bit)) ? 1 : 0);
	});

#define AXIS(v, code) do { if (state.v != last_state.v) write_event(uinput_fd, EV_ABS, code, state.v); } while(0)
	AXIS(axis_lx, ABS_X);
	AXIS(axis_ly, ABS_Y);
	AXIS(axis_rx, ABS_RX);
	AXIS(axis_ry, ABS_RY);
	AXIS(lz, ABS_Z);
	AXIS(rz, ABS_RZ);
	AXIS(hat_x, ABS_HAT0X);
	AXIS(hat_y, ABS_HAT0Y);

	write_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
	last_state = state;
}

VirtualGamepad::~VirtualGamepad()
{
	if (uinput_fd >= 0)
	{
		ioctl(uinput_fd, UI_DEV_DESTROY);
		close(uinput_fd);
	}
}

}