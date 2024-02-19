#include <iostream>
#include <asio.hpp>
#include "Listener.h"


int main() {

	ONET::NetworkManager::SetErrorCallback([](asio::error_code ec, unsigned err_line) {
		std::cout << "Asio error: '" << ec.message() << "' at line " << err_line << "\n";
		});
	ONET::ClientInterface net;
	std::string s;
	std::cout << "Client or host?: ";
	std::getline(std::cin, s);

	std::cout << "\n";

	ONET::NetworkManager::Init();
	if (!s.empty()) {
		if (std::tolower(s[0]) == 'c')
			net.connection.ConnectTo("127.0.0.1", 1234);
		else if (std::tolower(s[0] == 'h'))
			net.OpenToConnections(1234);
		else
			goto error;
	}
	else {
	error:
		std::cout << "Invalid input\n";
	}
	// Thread that allows user to input and send messages
	std::future<void> f = std::async(std::launch::async,
		[&] {
			std::string data = ".";
			while (!data.empty()) {
					//std::cout << "\nHOST: ";
					std::getline(std::cin, data);

					data = data + "\n";
					ONET::Message<ONET::MessageType> msg;
					for (int i = 0; i < data.size(); i++) {
						msg.content.push_back(static_cast<std::byte>(data[i]));
					}
					msg.header.size = data.size();
					net.connection.SendMsg(msg);
			}
		}
	);

	// Main thread waits, listens for and prints received messages to the console
	size_t size;
	while (true) {
		net.connection.m_incoming_msg_queue.Lock();
		size = net.connection.m_incoming_msg_queue.size();
		if ( size > 0) {
			auto msg = net.connection.m_incoming_msg_queue.pop_back();
			net.connection.m_incoming_msg_queue.Unlock();
			std::string s_content;

			for (auto byte : msg.content) {
				s_content.push_back(static_cast<char>(byte));
			}
			std::cout << "MESSAGE: " <<  s_content << "\n";
			s_content.clear();
		}
		net.connection.m_incoming_msg_queue.Unlock();

	}

	ONET::NetworkManager::Shutdown();
	//using namespace std::chrono_literals;
	//std::this_thread::sleep_for(200000ms);
}