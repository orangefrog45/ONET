#include <iostream>
#include "ONET.h"

using namespace ONET;

template<typename T>
concept NetType = requires(T t) {
	{ t.incoming_msg_queue };
};

template<NetType T>
void ProcessMessages(T& net) {
	size_t size = net.incoming_msg_queue.size();
	if (size > 0) {
		auto msg = net.incoming_msg_queue.pop_back();
		std::cout << msg.ContentAsString() << "\n";

		// Server sends message to other clients
		if constexpr (std::is_same_v<T, Server<MessageType>>) {
			net.BroadcastMessage(msg);
		}

	}
}


int main() {
	ONET::NetworkManager::SetErrorCallback([](asio::error_code ec, unsigned err_line) {
		std::cout << "Asio error: '" << ec.message() << "' at line " << err_line << "\n";
		});

	std::unique_ptr<ONET::ClientInterface<ClientServerMessageHeader<MessageType>>> client;
	std::unique_ptr<ONET::Server<ClientServerMessageHeader<MessageType>>> server;

	std::string s;
	std::cout << "Client or server?: ";

	std::getline(std::cin, s);


	if (!s.empty()) {
		if (std::tolower(s[0]) == 'c') {
			client = std::make_unique<ONET::ClientInterface<ClientServerMessageHeader<MessageType>>>();
			bool res = client->connection_tcp.ConnectTo("191.101.59.98", ONET_TCP_PORT);
			try {
				client->connection_udp.SetEndpoint("191.101.59.98", ONET_UDP_PORT);
				client->connection_udp.Open(ONET_UDP_PORT);
				client->connection_udp.ReadHeader();
			}
			catch (std::exception& e) {
				std::cout << "ERR: " << e.what() << "\n";
			}

			if (!res)
				std::cout << "Connection failed\n";
			else
			{
				std::cout << "Connection succeeded\n";
			}

		}
		else if (std::tolower(s[0] == 's')) {
			server = std::make_unique<ONET::Server<ClientServerMessageHeader<MessageType>>>();
			server->OpenToConnections(ONET_TCP_PORT);
		}
		else {
			goto error;
		}
	}
	else {
	error:
		std::cout << "Invalid input\n";
	}
	ONET::NetworkManager::Init();


	// Thread that allows user to input and send messages
	std::future<void> f;

	f = std::async(std::launch::async,
		[&] {
			std::string data = ".";
			while (!data.empty()) {
				std::getline(std::cin, data);

				data = data + "\n";
				ONET::Message<ClientServerMessageHeader<MessageType>> msg{ data, ONET::MessageType::STRING };

				if (server)
					server->BroadcastMessage(msg);
				else
					client->connection_udp.SendMsg(msg);
			}
		}
	);

	// Main thread waits, listens for and prints received messages to the console
	while (true) {
		if (client)
			ProcessMessages(*client);
		else {
			server->CheckConnectionsAlive();
			ProcessMessages(*server);
		}
	}

	using namespace std::chrono_literals;
	std::this_thread::sleep_for(200000ms);

	ONET::NetworkManager::Shutdown();
}