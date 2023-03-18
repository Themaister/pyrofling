/* Copyright (c) 2023 Hans-Kristian Arntzen
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

#include "client.hpp"
#include "listener.hpp"
#include <thread>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

using namespace PyroFling;

struct TestServer : HandlerFactoryInterface
{
	struct EchoRepeater : Handler
	{
		explicit EchoRepeater(Dispatcher &dispatcher_)
			: Handler(dispatcher_)
		{
		}

		bool handle(const FileHandle &fd, uint32_t) override
		{
			auto msg = parse_message(fd);
			if (!msg)
				return false;

			auto *echo = maybe_get<EchoPayloadMessage>(*msg);
			if (echo)
			{
				lseek(echo->fd.get_native_handle(), 0, SEEK_SET);
				char buf[1024] = {};
				read(echo->fd.get_native_handle(), buf, sizeof(buf) - 1);
				fprintf(stderr, "Got echo: \"%s\"\n", buf);
				return send_message(fd, MessageType::OK, msg->get_serial());
			}
			else
			{
				return send_message(fd, MessageType::ErrorProtocol, msg->get_serial());
			}
		}

		void release_id(uint32_t) override
		{
			fprintf(stderr, "Hanging up connection.\n");
			delete this;
		}
	};

	bool register_handler(Dispatcher &dispatcher, const FileHandle &fd, Handler *&handler) override
	{
		auto msg = parse_message(fd);
		if (!msg)
			return false;

		auto *client_hello = maybe_get<ClientHelloMessage>(*msg);
		if (!client_hello)
		{
			fprintf(stderr, "Did not get expected client hello message.\n");
			return false;
		}

		if (client_hello->wire.intent != ClientIntent::EchoStream)
		{
			fprintf(stderr, "Expected echo stream.\n");
			return false;
		}

		ServerHelloMessage::WireFormat hello = {};
		if (!send_wire_message(fd, client_hello->get_serial(), hello))
			return false;

		handler = new EchoRepeater{dispatcher};
		return true;
	}
};

int main()
{
	TestServer server;
	Dispatcher dispatcher{"/tmp/pyrofling-test-socket"};
	dispatcher.set_handler_factory_interface(&server);

	Client client{"/tmp/pyrofling-test-socket"};
	uint64_t serial;

	client.set_default_serial_handler([](const Message &msg) {
		fprintf(stderr, "Client: default reply (serial %llu, type %u).\n",
		        static_cast<unsigned long long>(msg.get_serial()), unsigned(msg.get_type()));
		return msg.get_type() == MessageType::OK;
	});

	ClientHelloMessage::WireFormat hello = {};
	hello.intent = ClientIntent::EchoStream;
	strncpy(hello.name, "TestApp", sizeof(hello.name));
	uint64_t hello_serial = client.send_wire_message(hello);

	std::thread thr{[&]() {
		while (dispatcher.iterate()) {}
	}};

	for (unsigned i = 0; i < 3; i++)
	{
		FileHandle fd{memfd_create("dummy", 0)};
		char buf[4] = "HAI";
		buf[3] = char('0' + i % 10);
		write(fd.get_native_handle(), buf, sizeof(buf));
		serial = client.send_file_handle_message(MessageType::EchoPayload, fd);

		if (!serial)
		{
			fprintf(stderr, "Failed to send message.\n");
		}
		else
		{
			client.set_serial_handler(serial, [](const Message &msg)
			{
				fprintf(stderr, "Got reply type: %u\n", unsigned(msg.get_type()));
				return msg.get_type() == MessageType::OK;
			});
		}
	}

	if (client.wait_plain_reply_for_serial(hello_serial) != MessageType::ServerHello)
		fprintf(stderr, "Failed to wait for serial.\n");

	if (!client.roundtrip())
		fprintf(stderr, "Failed to roundtrip.\n");

	dispatcher.kill();
	thr.join();
}
