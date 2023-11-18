#pragma once

#include "input.hpp"
#include "pyro_client.hpp"
#include <atomic>

namespace PyroFling
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
	void dispatch(const Granite::JoypadStateEvent &e) override;
};

void gamepad_main_poll_loop(PyroFling::PyroStreamClient *client, std::atomic_bool *done);
}