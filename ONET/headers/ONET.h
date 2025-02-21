#pragma once
#include <asio.hpp>
#include <string>
#include "CRC.h"
#include "tsQueue.h"
#include "tsVector.h"
#include <iostream>
#include <source_location>
#include <thread>

namespace ONET {
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define CONCAT_IMPL(x, y) x##y
#define FUNC_NAME __FUNCTION__ 
#define ONET_HANDLE_ERR(ec) NetworkManager::HandleError(ec);

#define ONET_UDP_PORT_CLIENT 47357
#define ONET_UDP_PORT_SERVER 47388
#define ONET_TCP_PORT 47389

// Prevent fragmentation by enforcing a maximum transmission size (required to handle corrupted packets)
#define ONET_MAX_DATAGRAM_SIZE_BYTES 504

	class NetworkManager {
	public:
		static NetworkManager* Get() {
			return mp_instance;
		}

		// Provide pointer to instance if using this singleton across DLL boundaries
		static void Init(NetworkManager* p_instance = nullptr) {
			if (!p_instance) {
				mp_instance = new NetworkManager();
				Get()->I_Init();
			}
			else {
				mp_instance = p_instance;
			}
		}

		static asio::io_context& GetIO() {
			return Get()->m_io;
		}

		static void Shutdown() {
			if (!Get()->mp_instance)
				return;

			Get()->I_Shutdown();
			delete Get()->mp_instance;
			Get()->mp_instance = nullptr;
		}

		static auto GetErrorCallback() {
			return Get()->error_callback;
		}

		static bool IsInitialized() {
			return static_cast<bool>(Get());
		}

		static void SetErrorCallback(const std::function<void(const asio::error_code&, const std::source_location& sl)>& cb) {
			Get()->error_callback = cb;
		}

		static bool IsNetThreadRunning() {
			return !Get()->m_net_thread.joinable();
		}

		static void HandleError(const asio::error_code& ec, const std::source_location& sl = std::source_location::current()) {
			Get()->I_HandleError(ec, sl);
		}


	private:
		inline static NetworkManager* mp_instance = nullptr;

		NetworkManager() = default;

		void I_Init() {
			m_running = true;

			m_net_thread = std::jthread([&]() { 
				while (m_running) { 
					asio::io_context::work idle_work{ m_io };
					m_io.run(); 
				} 
			});
		}

		void I_Shutdown() {
			m_running = false;
			m_io.stop();
			if (m_net_thread.joinable()) m_net_thread.join();
		}

		void I_HandleError(asio::error_code ec, std::source_location sl) {
			if (error_callback) error_callback(ec, sl);
		}

		std::function<void(asio::error_code, std::source_location sl)> error_callback = nullptr;

		bool m_running = false;
		asio::io_context m_io;
		std::jthread m_net_thread;

	};

	enum class MessageType {
		HANDSHAKE = 0,
		REQUEST_CLIENTS_INFO = 1,
		RESPONSE_CLIENTS_INFO = 2,
		STRING = 3,
		DATA
	};

	template<typename MsgTypeEnum>
	struct MessageHeader {
		size_t size=0;
		uint16_t checksum;
		MsgTypeEnum message_type;

		using enum_type = MsgTypeEnum;
	};

	template<typename MsgTypeEnum>
	struct ClientServerMessageHeader : public MessageHeader<MsgTypeEnum> {
		ClientServerMessageHeader() {
			static_assert((uint32_t)MsgTypeEnum::HANDSHAKE == 0, "MsgTypeEnum must have member 'HANDSHAKE' equal to 0");
			static_assert((uint32_t)MsgTypeEnum::REQUEST_CLIENTS_INFO == 1, "MsgTypeEnum must have member 'REQUEST_CLIENTS_INFO' equal to 1");
			static_assert((uint32_t)MsgTypeEnum::RESPONSE_CLIENTS_INFO == 2, "MsgTypeEnum must have member 'RESPONSE_CLIENTS_INFO' equal to 2");
			static_assert((uint32_t)MsgTypeEnum::STRING == 3, "MsgTypeEnum must have member 'STRING' equal to 3");
		}

		uint64_t connection_id;
	};


	template<typename MsgHeaderType>
	struct MessageTCP {
		MessageTCP() {
		}

		MessageTCP(const std::string& msg_content, MsgHeaderType::enum_type msg_type) {
			content.resize(msg_content.size());
			std::ranges::copy(msg_content, reinterpret_cast<char*>(content.data()));
			header.size = content.size();
			header.message_type = msg_type;
		}

		explicit MessageTCP(MsgHeaderType::enum_type msg_type) {
			header.message_type = msg_type;
		}

		// Returns content of message interpreted as a string
		inline std::string ContentAsString() {
			return {reinterpret_cast<const char*>(content.data()), content.size()};
		}

		using HeaderT = MsgHeaderType;

		MsgHeaderType header;
		std::vector<std::byte> content;
	};

	template<typename MsgHeaderType>
	struct MessageUDP {
		MessageUDP() {
		}

		explicit MessageUDP(MsgHeaderType::enum_type msg_type) {
			header.message_type = msg_type;
		}

		// Returns content of message interpreted as a string
		inline std::string ContentAsString() {
			return { reinterpret_cast<const char*>(content.data()), content.size() };
		}

		using HeaderT = MsgHeaderType;

		MsgHeaderType header;
		std::array<std::byte, ONET_MAX_DATAGRAM_SIZE_BYTES - sizeof(MsgHeaderType)> content;
	};



	template<typename MessageType>
	class SocketConnectionBase {
	public:
		SocketConnectionBase(tsQueue<MessageType>& incoming_queue, const std::function<void()>& _MessageReceiveCallback = nullptr) : incoming_msg_queue(incoming_queue), MessageReceiveCallback(_MessageReceiveCallback) {
		}

		tsQueue<MessageType>& incoming_msg_queue;

		void OnMessageReceive() const {
			if (MessageReceiveCallback) MessageReceiveCallback();
		}

	private:
		std::function<void()> MessageReceiveCallback = nullptr;
	};


	template<typename MsgHeaderType>
	class SocketConnectionTCP : public SocketConnectionBase<MessageTCP<MsgHeaderType>> {
	public:
		SocketConnectionTCP(tsQueue<MessageTCP<MsgHeaderType>>& incoming_queue, std::function<void()> _MessageReceiveCallback = nullptr) : 
			SocketConnectionBase<MessageTCP<MsgHeaderType>>(incoming_queue, _MessageReceiveCallback) {
			m_socket = std::make_unique<asio::ip::tcp::socket>(NetworkManager::GetIO());
		};

		bool ConnectTo(const std::string& ipv4, uint_least16_t port) {
			asio::error_code ec;

			m_socket->connect(asio::ip::tcp::endpoint{ asio::ip::make_address(ipv4), asio::ip::port_type{port} }, ec);
			if (ec) {
				ONET_HANDLE_ERR(ec);
				return false;
			}

			if (m_socket->is_open()) {
				ReadHeader();
			}
			else {
				return false;
			}


			return true;
		}

		void Disconnect() {
#ifdef ONET_DEBUG
			std::cout << "TCP socket disconnecting\n";
#endif
			if (IsConnected()) {
				m_socket->close();
			}
		}

		bool IsConnected() {
			return m_socket->is_open();
		}

		void SendMsg(MessageTCP<MsgHeaderType>& msg) {
			msg.header.size = msg.content.size();

			asio::post(NetworkManager::GetIO(),
				[=, this]() {
					bool currently_writing_msg;
					{
						std::scoped_lock l(m_outgoing_msg_queue.m_queue_mux);
						currently_writing_msg = !m_outgoing_msg_queue.GetQueue().empty();
						m_outgoing_msg_queue.GetQueue().push_back(msg);
					}

					if (!currently_writing_msg) {
						WriteHeader();
					}
				}
			);
		}

		//ASYNC
		void WriteBody() {
			std::scoped_lock l(m_outgoing_msg_queue.m_queue_mux);
			asio::async_write(*m_socket, asio::buffer(m_outgoing_msg_queue.GetQueue()[0].content.data(), m_outgoing_msg_queue.GetQueue()[0].content.size()),
				[this](std::error_code ec, std::size_t s) {
					if (!ec) {
						m_outgoing_msg_queue.pop_front();

						if (!m_outgoing_msg_queue.empty())
							WriteHeader();
					}
					else {
						ONET_HANDLE_ERR(ec);
					}
				}
			);
		}

		// ASYNC
		void WriteHeader() {
			std::scoped_lock l(m_outgoing_msg_queue.m_queue_mux);
			asio::async_write(*m_socket, asio::buffer(&m_outgoing_msg_queue.GetQueue()[0].header, sizeof(MsgHeaderType)),
				[this](std::error_code ec, std::size_t) {
					if (!ec) {
						if (m_outgoing_msg_queue.front().content.size() > 0) {
							WriteBody();
						}
						else {
							m_outgoing_msg_queue.pop_front();

							if (!m_outgoing_msg_queue.empty())
								WriteHeader();
						}
					}
					else {
						ONET_HANDLE_ERR(ec);
					}
				}
			);
		}
		
		// ASYNC
		void ReadBody() {
			asio::async_read(*m_socket, asio::buffer(m_temp_receiving_message.content.data(), m_temp_receiving_message.content.size()),
				[this](std::error_code ec, std::size_t) {
					if (!ec) {
						this->incoming_msg_queue.push_back(m_temp_receiving_message);
						this->OnMessageReceive();
						ReadHeader();
					}
					else {
						ONET_HANDLE_ERR(ec);
					}
				}
			);
			
		}
		
		// ASYNC
		void ReadHeader() {
			asio::async_read(*m_socket, asio::buffer(&m_temp_receiving_message.header, sizeof(MsgHeaderType)),
				[this](std::error_code ec, std::size_t) {
#ifdef ONET_DEBUG
					std::cout << "READING TCP\n";
#endif
					if (!ec) {
						if (m_temp_receiving_message.header.size > 0) {
							m_temp_receiving_message.content.resize(m_temp_receiving_message.header.size);
							ReadBody();
						}
						else {
							this->incoming_msg_queue.push_back(m_temp_receiving_message);
							this->OnMessageReceive();
							ReadHeader();
						}
					}
					else {
						if (ec == asio::error::eof || ec == asio::error::connection_reset) {
							Disconnect();
						}
						else {
							ONET_HANDLE_ERR(ec);
						}
					}
				}
			);
		}

		auto& GetSocket() {
			return *m_socket;
		}

		asio::ip::tcp::endpoint GetRemoteEndpoint() {
			return m_socket->remote_endpoint();
		}

	private:
		std::unique_ptr<asio::ip::tcp::socket> m_socket;

		tsQueue<MessageTCP<MsgHeaderType>> m_outgoing_msg_queue;

		MessageTCP<MsgHeaderType> m_temp_receiving_message;
	};

	template<typename MsgHeaderType>
	class SocketConnectionUDP : public SocketConnectionBase<MessageUDP<MsgHeaderType>> {
	public:
		SocketConnectionUDP(tsQueue<MessageUDP<MsgHeaderType>>& incoming_queue, std::function<void()> _MessageReceiveCallback = nullptr) : 
			SocketConnectionBase<MessageUDP<MsgHeaderType>>(incoming_queue, _MessageReceiveCallback) {
			m_socket = std::make_unique<asio::ip::udp::socket>(NetworkManager::GetIO());
		}

		void Open(uint_least16_t port, const std::optional<std::string>& address = std::nullopt) {
			m_socket->open(asio::ip::udp::v4());
			if (address.has_value())
				m_socket->bind(asio::ip::udp::endpoint{ asio::ip::make_address(address.value()), port });
			else
				m_socket->bind(asio::ip::udp::endpoint{ asio::ip::make_address("0.0.0.0"), port });

			m_socket->set_option(asio::socket_base::receive_buffer_size(5'000'000));
			m_socket->set_option(asio::socket_base::send_buffer_size(5'000'000));
		}

		void SetEndpoint(const std::string& ipv4, unsigned port) {
			m_endpoint = asio::ip::udp::endpoint(asio::ip::make_address(ipv4), port);
		}

		void SendMsg(MessageUDP<MsgHeaderType>& msg) {
			msg.header.size = msg.content.size();
			msg.header.checksum = CRC::Calculate(msg.content.data(), msg.content.size(), CRC::CRC_16_ARC());

			asio::post(NetworkManager::GetIO(),
				[=, this]() {
					bool currently_writing_msg;
					{
						std::scoped_lock l(m_outgoing_msg_queue.m_queue_mux);
						currently_writing_msg = !m_outgoing_msg_queue.GetQueue().empty();
						m_outgoing_msg_queue.GetQueue().push_back(msg);
					}

					if (!currently_writing_msg) {
						WriteMessage();
					}
				}
			);
		}


		// ASYNC
		void WriteMessage() {
			std::scoped_lock l(m_outgoing_msg_queue.m_queue_mux);
			m_socket->async_send_to(asio::buffer(&m_outgoing_msg_queue.GetQueue()[0], sizeof(MessageUDP<MsgHeaderType>)), m_endpoint,
				[this](std::error_code ec, std::size_t size) {
					if (!ec) {
#ifdef ONET_DEBUG
						std::cout << "Popping outgoing message\n";
#endif
						m_outgoing_msg_queue.pop_front();

						if (!m_outgoing_msg_queue.empty())
							WriteMessage();
					}
					else {
						ONET_HANDLE_ERR(ec);
					}
				}
			);

		}



		// ASYNC
		void ReadMessage() {
			m_socket->async_receive_from(asio::buffer(
				&m_temp_receiving_message,
				sizeof(MessageUDP<MsgHeaderType>)), m_receiving_endpoint,
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						m_current_bytes_read += length;
						if (m_current_bytes_read == sizeof(MessageUDP<MsgHeaderType>)) {
#ifdef ONET_DEBUG
							std::cout << "Validating body checksum\n";
#endif						
							uint16_t crc = CRC::Calculate(m_temp_receiving_message.content.data(),  m_temp_receiving_message.content.size(), CRC::CRC_16_ARC());
							if (m_temp_receiving_message.header.checksum == crc) {
#ifdef ONET_DEBUG
								std::cout << "ReadBody pushing incoming msg\n";
#endif
								this->incoming_msg_queue.push_back(m_temp_receiving_message);
								this->OnMessageReceive();
							}
							else {
								std::cout << "UDP checksum validation failed, from header:" << m_temp_receiving_message.header.checksum <<
									", local crc: " << crc << ", discarding\n";
							}
							m_current_bytes_read = 0;
						}
						ReadMessage();
					}
					else {
						m_current_bytes_read = 0;
						ONET_HANDLE_ERR(ec);
					}
				}
			);

		}
	

		void Disconnect() {
#ifdef ONET_DEBUG
			std::cout << "UDP socket disconnecting";
#endif
			m_socket->close();
		}

		auto& GetSocket() {
			return *m_socket;
		}

	private:
		std::unique_ptr<asio::ip::udp::socket> m_socket;

		asio::ip::udp::endpoint m_endpoint;

		asio::ip::udp::endpoint m_receiving_endpoint;

		size_t m_current_bytes_read = 0;

		tsQueue<MessageUDP<MsgHeaderType>> m_outgoing_msg_queue;

		MessageUDP<MsgHeaderType> m_temp_receiving_message;

	};


	template<typename MsgHeaderType>
	class ClientInterface {
	public:
		ClientInterface() {
			static_assert(std::derived_from<MsgHeaderType, ClientServerMessageHeader<typename MsgHeaderType::enum_type>>, "MsgHeaderType must be derived from ClientServerMessageHeader");
		}

		void Disconnect() {
			connection_tcp.Disconnect();
			connection_udp.Disconnect();
		}

		uint64_t GetConnectionID() const {
			return m_connection_id;
		}

		void OnUpdate() {
			if (m_connection_id != 0)
				return;

			std::scoped_lock(incoming_msg_queue_tcp.m_queue_mux);
			if (!incoming_msg_queue_tcp.empty() && incoming_msg_queue_tcp.front().header.message_type == MsgHeaderType::enum_type::HANDSHAKE) {
				auto msg = incoming_msg_queue_tcp.pop_front();
				// This is the initial handshake, server gives connection a unique ID, process and discard.
				m_connection_id = *reinterpret_cast<uint64_t*>(msg.content.data());
				std::cout << "ID RECEIVED: " << m_connection_id << "\n";
			}
		}

		tsQueue<MessageTCP<MsgHeaderType>> incoming_msg_queue_tcp;
		tsQueue<MessageUDP<MsgHeaderType>> incoming_msg_queue_udp;
		SocketConnectionTCP<MsgHeaderType> connection_tcp{ incoming_msg_queue_tcp };
		SocketConnectionUDP<MsgHeaderType> connection_udp{ incoming_msg_queue_udp };
	private:
		uint64_t m_connection_id = 0;
	};

	template<typename MsgHeaderType>
	struct ServerConnection {
		ServerConnection(std::shared_ptr<SocketConnectionTCP<MsgHeaderType>> _tcp, std::shared_ptr<SocketConnectionUDP<MsgHeaderType>> _udp, uint64_t _connection_id) : tcp(std::move(_tcp)), udp(std::move(_udp)), connection_id(_connection_id)  {};
		std::shared_ptr<SocketConnectionTCP<MsgHeaderType>> tcp;
		std::shared_ptr<SocketConnectionUDP<MsgHeaderType>> udp;
		uint64_t connection_id;
	};

	template<typename MsgHeaderType>
	class Server {
	public:
		Server() {
			static_assert(std::derived_from<MsgHeaderType, ClientServerMessageHeader<typename MsgHeaderType::enum_type>>, "MsgHeaderType must be derived from ClientServerMessageHeader");
		}

		void OpenToConnections(unsigned port) {
			m_udp_receiver.Open(ONET_UDP_PORT_SERVER);
			m_udp_receiver.ReadMessage();

			m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(NetworkManager::GetIO(), asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));
			
			auto tcp_socket = std::make_shared<SocketConnectionTCP<MsgHeaderType>>(incoming_msg_queue_tcp);
			
			m_acceptor->async_accept(tcp_socket->GetSocket(), std::bind(&Server::HandleConnectionRequest, this, std::placeholders::_1, tcp_socket));
		}
		
		void OnUpdate() {
			std::scoped_lock l(incoming_msg_queue_tcp.m_queue_mux);
			bool pop = false;

			// Check only TCP queue for these messages as they will always be sent via this protocol
			auto& queue = incoming_msg_queue_tcp.GetQueue();
			if (!queue.empty() && queue.front().header.message_type == MsgHeaderType::enum_type::REQUEST_CLIENTS_INFO) {
				for (auto& connection : connections.GetVector()) {
					if (connection.connection_id == queue.front().header.connection_id) {
						MessageTCP<MsgHeaderType> msg{ std::to_string(connections.size()), MsgHeaderType::enum_type::RESPONSE_CLIENTS_INFO };
						connection.tcp->SendMsg(msg);
						pop = true;
						break;
					}
				}
			}

			if (pop)
				incoming_msg_queue_tcp.pop_front();
		}

		
		void HandleConnectionRequest(const asio::error_code& ec, std::shared_ptr<SocketConnectionTCP<MsgHeaderType>> tcp_socket) {
			asio::ip::tcp::endpoint tcp_endpoint = tcp_socket->GetRemoteEndpoint();

			// Get IP
			std::string address_str = tcp_endpoint.address().to_string();
			size_t ip_start_pos = address_str.rfind(':') + 1;
			address_str = address_str.substr(ip_start_pos, address_str.size() - ip_start_pos);

			std::cout << "Connection accepted from: " << address_str << "\n";

			std::scoped_lock l(connections.GetMutex());

			unsigned endpoint_port = ONET_UDP_PORT_CLIENT;
			unsigned server_port = ONET_UDP_PORT_SERVER;
			for (auto& connection : connections.GetVector()) {
				if (connection.tcp->IsConnected() && connection.tcp->GetRemoteEndpoint().address() == tcp_endpoint.address()) {
					// This will only happen during multi-client testing on a single machine
					// In a 2-client test setup, the second client to connect to the server should be listening to ONET_UDP_PORT_CLIENT - 1 instead
					endpoint_port--;
					server_port--;
				}
			}

			std::shared_ptr<SocketConnectionUDP<MsgHeaderType>> udp_socket = std::make_shared<SocketConnectionUDP<MsgHeaderType>>(incoming_msg_queue_udp);
			udp_socket->SetEndpoint(address_str, endpoint_port);
			if (server_port != ONET_UDP_PORT_SERVER) {
				udp_socket->Open(server_port);
			}

			udp_socket->ReadMessage();
			tcp_socket->ReadHeader();

			ServerConnection<MsgHeaderType> connection(tcp_socket, udp_socket, GenConnectionID());
			// Send initial handshake message to give connection a unique identifier.
			MessageTCP<MsgHeaderType> msg;
			msg.header.message_type = MsgHeaderType::enum_type::HANDSHAKE;
			msg.content.resize(sizeof(uint64_t));
			msg.header.size = msg.content.size();
			std::memcpy(msg.content.data(), &connection.connection_id, sizeof(uint64_t));

			tcp_socket->SendMsg(msg);

			connections.GetVector().push_back(std::move(connection));

			auto new_socket = std::make_shared<SocketConnectionTCP<MsgHeaderType>>(incoming_msg_queue_tcp);

			m_acceptor->async_accept(new_socket->GetSocket(), std::bind(&Server::HandleConnectionRequest, this, std::placeholders::_1, new_socket));
		}

		void BroadcastMessageTCP(MessageTCP<MsgHeaderType>& msg, uint64_t connection_id_to_ignore = 0) {
			std::scoped_lock l(connections.GetMutex());

			for (auto& connection : connections.GetVector()) {
				if (connection.connection_id == connection_id_to_ignore)
					continue;

				connection.tcp->SendMsg(msg);
			}
		}

		void BroadcastMessageUDP(MessageUDP<MsgHeaderType>& msg, uint64_t connection_id_to_ignore = 0) {
			std::scoped_lock l(connections.GetMutex());

			for (auto& connection : connections.GetVector()) {
				if (connection.connection_id == connection_id_to_ignore)
					continue;

				connection.udp->SendMsg(msg);
			}

		}

		void CheckConnectionsAlive() {
			std::scoped_lock l(connections.GetMutex());
			auto& vec = connections.GetVector();
			
			for (int i = 0; i < connections.GetVector().size(); i++) {
				if (!vec[i].tcp->IsConnected()) {
					std::cout << "Erasing connection\n";
					vec[i].udp->Disconnect();
					vec[i].tcp->Disconnect();
					vec.erase(vec.begin() + i);
					i--;
				}
			}

		}

		tsQueue<MessageTCP<MsgHeaderType>> incoming_msg_queue_tcp;
		tsQueue<MessageUDP<MsgHeaderType>> incoming_msg_queue_udp;

		tsVector<ServerConnection<MsgHeaderType>> connections;
	private:
		SocketConnectionUDP<MsgHeaderType> m_udp_receiver{ incoming_msg_queue_udp };

		uint64_t GenConnectionID() {
			static uint64_t current_id = 1;

			std::scoped_lock l(m_current_connection_id_mux);
			uint64_t id = current_id;
			current_id++;

			return id;
		}

		std::recursive_mutex m_current_connection_id_mux;

		std::recursive_mutex connection_mux;

		std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;

	};
}