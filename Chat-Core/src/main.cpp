#include <iostream>
#include <asio.hpp>
#include "ONET.h"


template<typename T>
concept NetType = requires(T t) {
	{ t.incoming_msg_queue };
};

template<NetType T>
void ProcessMessages(T& net) {
	size_t size = net.incoming_msg_queue.size();
	if (size > 0) {
		auto msg = net.incoming_msg_queue.pop_back();
		std::string s_content;

		for (auto byte : msg.content) {
			s_content.push_back(static_cast<char>(byte));
		}
		std::cout << "MESSAGE: " << s_content << "\n";
		s_content.clear();
	}
}


int main() {

	ONET::NetworkManager::SetErrorCallback([](asio::error_code ec, unsigned err_line) {
		std::cout << "Asio error: '" << ec.message() << "' at line " << err_line << "\n";
		});
	std::unique_ptr<ONET::ClientInterface<ONET::MessageType>> client;
	std::unique_ptr<ONET::Server<ONET::MessageType>> server;

	std::string s;
	std::cout << "Client or host?: ";

	std::getline(std::cin, s);


	if (!s.empty()) {
		if (std::tolower(s[0]) == 'c') {
			client = std::make_unique<ONET::ClientInterface<ONET::MessageType>>();
			bool res = client->connection.ConnectTo("191.101.59.98", 1234);

			//bool res = net.connection.ConnectTo("90.243.42.172", 1234);
			if (!res)
				std::cout << "Connection failed\n";
			else
			{
				std::cout << "Connection succeeded\n";
			}

		}
		else if (std::tolower(s[0] == 'h')) {
			std::cout << "h hit";
			server = std::make_unique<ONET::Server<ONET::MessageType>>();
			server->OpenToConnections(1234);
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
				ONET::Message<ONET::MessageType> msg;
				for (int i = 0; i < data.size(); i++) {
					msg.content.push_back(static_cast<std::byte>(data[i]));
				}
				msg.header.size = data.size();

				if (server)
					server->BroadcastMessage(msg);
				else
					client->connection.SendMsg(msg);
			}
		}
	);

	// Main thread waits, listens for and prints received messages to the console
	while (true) {
		if (client)
			ProcessMessages(*client);
		else 
			ProcessMessages(*server);
	}

	using namespace std::chrono_literals;
	std::this_thread::sleep_for(200000ms);

	ONET::NetworkManager::Shutdown();
}