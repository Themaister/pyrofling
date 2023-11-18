#include "cli_parser.hpp"
#include "string_helpers.hpp"
#include "pyro_client.hpp"
#include "pad_handler.hpp"

using namespace Granite;

static void print_help()
{
	LOGI("pyrofling-gamepad ip:port\n");
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

	PyroFling::gamepad_main_poll_loop(&pyro, nullptr);
}
