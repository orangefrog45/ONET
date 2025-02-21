#include "ONET.h"

using namespace ONET;

void ProcessMessages(Server<ClientServerMessageHeader<MessageType>>& net) {
	{
		std::scoped_lock l{ net.incoming_msg_queue_tcp.mux };
		auto& tcp_queue = net.incoming_msg_queue_tcp.GetQueue();
		for (auto& msg : tcp_queue) {
			net.BroadcastMessageTCP(msg, msg.header.connection_id);
		}
		tcp_queue.clear();
	}

	{
		std::scoped_lock l{ net.incoming_msg_queue_udp.mux };
		auto& udp_queue = net.incoming_msg_queue_udp.GetQueue();
		for (auto& msg : udp_queue) {
			net.BroadcastMessageUDP(msg, msg.header.connection_id);
		}
		udp_queue.clear();
	}
}


int main() {
	ONET::NetworkManager::Init();
	ONET::NetworkManager::SetErrorCallback([](const asio::error_code& ec, const std::source_location& sl) {
		std::cout << "Asio error: '" << ec.message() << "' at file " << sl.function_name() << " at line " << sl.line() << "\n";
		});

	ONET::Server<ClientServerMessageHeader<MessageType>> server{};
	server.OpenToConnections(ONET_TCP_PORT);

	while (true) {
		server.CheckConnectionsAlive();
		server.OnUpdate();
		ProcessMessages(server);
	}

	ONET::NetworkManager::Shutdown();
}