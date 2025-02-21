#include "ONET.h"
#include <chrono>

using namespace ONET;

template<typename T>
concept NetType = requires(T t) {
	{ t.incoming_msg_queue };
};
enum class GameMessageType {
	HANDSHAKE = 0,
	REQUEST_CLIENTS_INFO = 1,
	RESPONSE_CLIENTS_INFO = 2,
	STRING = 3,
	BLOCK_DATA,
	CONNECTION_ALERT,
	GAME_EVENT,
	WEAPON_DATA,
	CORE_BLOCK_EVENT,
	PHYSICS_SYNC,
};

unsigned g_total_udp_messages = 0;

void ProcessMessages(Server<ClientServerMessageHeader<GameMessageType>>& net) {
	size_t size_tcp = net.incoming_msg_queue_tcp.size();
	if (size_tcp > 0) {
		auto msg = net.incoming_msg_queue_tcp.pop_front();

		// Server sends message to other clients
		net.BroadcastMessageTCP(msg, msg.header.connection_id);
	}

	size_t size_udp = net.incoming_msg_queue_udp.size();
	g_total_udp_messages += size_udp;

	net.incoming_msg_queue_udp.m_queue_mux.lock();
	auto& udp_queue = net.incoming_msg_queue_udp.GetQueue();
	for (auto& msg : udp_queue) {
		net.BroadcastMessageUDP(msg, msg.header.connection_id);
	}
	udp_queue.clear();
	net.incoming_msg_queue_udp.m_queue_mux.unlock();
}


int main() {
	using namespace std::chrono_literals;

	ONET::NetworkManager::Init();
	ONET::NetworkManager::SetErrorCallback([](const asio::error_code& ec, const std::source_location& sl) {
		std::cout << "Asio error: '" << ec.message() << "' at file " << sl.function_name() << " at line " << sl.line() << "\n";
		});

	ONET::Server<ClientServerMessageHeader<GameMessageType>> server{};
	server.OpenToConnections(ONET_TCP_PORT);

	auto last_sync_time = std::chrono::high_resolution_clock::now();

	while (true) {
		server.CheckConnectionsAlive();
		server.OnUpdate();
		ProcessMessages(server);

		auto now = std::chrono::high_resolution_clock::now();

		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync_time).count() > 1000.f) {
			last_sync_time = now;
			std::cout << g_total_udp_messages << "\n";
			g_total_udp_messages = 0;
		}
	}

	ONET::NetworkManager::Shutdown();
}