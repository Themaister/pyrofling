#include "simple_socket.hpp"
#include "listener.hpp"
#include "messages.hpp"
#include <vector>
#include <thread>
#include <algorithm>
#include <mutex>
#include <string.h>

#include <sys/socket.h>
#include <netdb.h>

using namespace PyroFling;

struct Server : HandlerFactoryInterface
{
	struct Connection : Handler
	{
		explicit Connection(Dispatcher &dispatcher_, Server &server_) : Handler(dispatcher_), server(server_) {}
		Server &server;
		RemoteAddress tcp_remote;
		RemoteAddress udp_remote;
		uint64_t cookie = 0;
		uint64_t client_cookie = 0;

		std::string tcp_receive_buffer;

		bool handle(const FileHandle &fd, uint32_t) override
		{
			char buffer[8];

			if (tcp_receive_buffer.size() >= 8)
				return false;

			size_t ret = receive_stream_message(fd, buffer, sizeof(buffer) - tcp_receive_buffer.size());
			if (!ret)
				return false;

			tcp_receive_buffer.insert(tcp_receive_buffer.end(), buffer, buffer + ret);
			size_t n;

			while ((n = tcp_receive_buffer.find_first_of('\n')) != std::string::npos)
			{
				auto cmd = tcp_receive_buffer.substr(0, n);
				tcp_receive_buffer = tcp_receive_buffer.substr(n + 1);

				if (cmd == "PYRO1")
				{
					if (!send_stream_message(fd, &cookie, sizeof(cookie)))
						return false;
				}
				else if (cmd == "COOKIE")
				{
					if (!send_stream_message(fd, &client_cookie, sizeof(client_cookie)))
						return false;
				}
				else
					return false;
			}

			return true;
		}

		void release_id(uint32_t) override
		{
			std::lock_guard<std::mutex> holder{server.lock};
			auto itr = std::find_if(server.connections.begin(), server.connections.end(),
			                        [this](const std::unique_ptr<Connection> &ptr) {
				                        return ptr.get() == this;
			                        });
			if (itr != server.connections.end())
				server.connections.erase(itr);
		}

		void write_udp(const void *data, size_t size)
		{
			if (udp_remote)
				dispatcher.write_udp_datagram(udp_remote, "HEADER", 6, data, size);
		}
	};

	bool register_handler(Dispatcher &, const FileHandle &, Handler *&) override
	{
		return false;
	}

	bool register_tcp_handler(Dispatcher &dispatcher, const FileHandle &, const RemoteAddress &remote,
	                          Handler *&handler) override
	{
		{
			char host[64] = {};
			char serv[64] = {};
			getnameinfo(reinterpret_cast<const sockaddr *>(&remote.addr), remote.addr_size,
			            host, sizeof(host), serv, sizeof(serv), NI_DGRAM);

			fprintf(stderr, "TCP: Host: \"%s\", Serv: \"%s\"\n", host, serv);
		}

		auto conn = std::make_unique<Connection>(dispatcher, *this);
		conn->tcp_remote = remote;
		conn->cookie = ++cookie;
		handler = conn.get();
		std::lock_guard<std::mutex> holder{lock};
		connections.push_back(std::move(conn));
		return true;
	}

	void write_udp(const void *data, size_t size)
	{
		std::lock_guard<std::mutex> holder{lock};
		for (auto &conn : connections)
			conn->write_udp(data, size);
	}

	void handle_udp_datagram(Dispatcher &, const RemoteAddress &remote,
	                         const void *msg, unsigned size) override
	{
		if (size == 2 * sizeof(uint64_t))
		{
			uint64_t c[2];
			memcpy(c, msg, sizeof(c));

			char host[64] = {};
			char serv[64] = {};
			getnameinfo(reinterpret_cast<const sockaddr *>(&remote.addr), remote.addr_size,
						host, sizeof(host), serv, sizeof(serv), NI_DGRAM);

			fprintf(stderr, "Host: \"%s\", Serv: \"%s\"\n", host, serv);

			for (auto &conn : connections)
			{
				if (conn->cookie == c[0] && !conn->udp_remote)
				{
					conn->udp_remote = remote;
					conn->client_cookie = c[1];
					break;
				}
			}
		}
	}

	uint64_t cookie = 1000;
	std::mutex lock;
	std::vector<std::unique_ptr<Connection>> connections;
};

int main()
{
	Dispatcher::block_signals();
	Server server;
	Dispatcher dispatcher("/tmp/pyro", "8080");
	dispatcher.set_handler_factory_interface(&server);
	std::thread thr([&dispatcher]() { while (dispatcher.iterate()); });
	std::thread sender([&server]() {
		for (unsigned i = 0; i < 64; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			server.write_udp(" OHAI", 5);
		}
	});

	Socket tcp, udp, tcp2, udp2;
	if (!tcp.connect(Socket::Proto::TCP, "localhost", "8080"))
		return EXIT_FAILURE;
	if (!udp.connect(Socket::Proto::UDP, "127.0.0.1", "8080"))
		return EXIT_FAILURE;
	if (!tcp2.connect(Socket::Proto::TCP, "localhost", "8080"))
		return EXIT_FAILURE;
	if (!udp2.connect(Socket::Proto::UDP, "127.0.0.1", "8080"))
		return EXIT_FAILURE;

	if (!tcp.write("PYRO1\n", 6))
		return EXIT_FAILURE;
	if (!tcp2.write("PYRO1\n", 6))
		return EXIT_FAILURE;

	uint64_t cookie[2];
	if (!tcp.read(&cookie[0], sizeof(cookie[0])))
		return EXIT_FAILURE;
	if (!tcp2.read(&cookie[1], sizeof(cookie[1])))
		return EXIT_FAILURE;

	for (unsigned j = 0; j < 2; j++)
	{
		for (unsigned i = 0; i < 8; i++)
		{
			uint64_t cs[] = { cookie[j], 100 + j };
			auto &u = j ? udp2 : udp;
			auto &t = j ? tcp2 : tcp;
			if (!u.write(cs, sizeof(cs)))
				return EXIT_FAILURE;

			if (!t.write("COOKIE\n", 7))
				return EXIT_FAILURE;

			uint64_t client_cookie;
			if (!t.read(&client_cookie, sizeof(client_cookie)))
				return EXIT_FAILURE;

			if (client_cookie == cs[1])
				break;
			else if (client_cookie != 0)
				return EXIT_FAILURE;
		}
	}

	uint8_t buffer[1024];

	for (;;)
	{
		size_t len = udp.read_partial(buffer, sizeof(buffer) - 1);
		if (len == 0)
			break;
		buffer[len] = '\0';
		fprintf(stderr, "Conn #1: Got reply: \"%s\"\n", buffer);

		len = udp2.read_partial(buffer, sizeof(buffer) - 1);
		if (len == 0)
			break;
		buffer[len] = '\0';
		fprintf(stderr, "Conn #2: Got reply: \"%s\"\n", buffer);
	}

	dispatcher.kill();
	sender.join();
	thr.join();
}
