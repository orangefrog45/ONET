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

#define ONET_UDP_PORT 1235
#define ONET_TCP_PORT 1234

// Prevent fragmentation by enforcing a maximum transmission size (required to handle corrupted packets)
#define ONET_MAX_DATAGRAM_SIZE_BYTES 508

	class NetworkManager {
	public:
		static NetworkManager& Get() {
			static NetworkManager s_instance;
			return s_instance;
		}

		static void Init() {
			Get().I_Init();
		}

		static asio::io_context& GetIO() {
			return Get().m_io;
		}

		static void Shutdown() {
			Get().I_Shutdown();
		}

		static auto GetErrorCallback() {
			return Get().error_callback;
		}

		static void SetErrorCallback(const std::function<void(const asio::error_code&, const std::source_location& sl)>& cb) {
			Get().error_callback = cb;
		}

		static bool IsNetThreadRunning() {
			return !Get().m_net_thread.joinable();
		}

		static void HandleError(const asio::error_code& ec, const std::source_location& sl = std::source_location::current()) {
			Get().I_HandleError(ec, sl);
		}


	private:
		NetworkManager() = default;

		void I_Init() {
			asio::io_context::work idle_work{ m_io };
			m_net_thread = std::jthread([&]() { m_io.run(); });
		}

		void I_Shutdown() {
			m_io.stop();
			if (m_net_thread.joinable()) m_net_thread.join();
		}

		void I_HandleError(asio::error_code ec, std::source_location sl) {
			if (error_callback) error_callback(ec, sl);
		}

		std::function<void(asio::error_code, std::source_location sl)> error_callback = nullptr;

		asio::io_context m_io;
		std::jthread m_net_thread;

	};

	enum class MessageType {
		HANDSHAKE = 0,
		REQUEST_CLIENTS_INFO = 1,
		RESPONSE_CLIENTS_INFO = 2,
		STRING = 3,
	};

	template<typename MsgTypeEnum>
	struct MessageHeader {
		uint64_t size=0;
		uint32_t body_checksum;
		MsgTypeEnum message_type;

		using enum_type = MsgTypeEnum;
	};

	template<typename MsgTypeEnum>
	struct ClientServerMessageHeader : public MessageHeader<MsgTypeEnum> {
		ClientServerMessageHeader() {
			static_assert((uint32_t)MsgTypeEnum::HANDSHAKE == 0, "MsgTypeEnum must have member 'HANDSHAKE' equal to 0");
			static_assert((uint32_t)MsgTypeEnum::REQUEST_CLIENTS_INFO == 1, "MsgTypeEnum must have member 'REQUEST_CLIENTS_INFO' equal to 1");
			static_assert((uint32_t)MsgTypeEnum::RESPONSE_CLIENTS_INFO == 2, "MsgTypeEnum must have member 'RESPONSE_CLIENTS_INFO' equal to 2");
		}

		uint64_t connection_id;
		float time_sent;
	};


	template<typename MsgHeaderType>
	struct Message {
		Message() {
		}


		Message(const std::string& msg_content, MsgHeaderType::enum_type msg_type) {
			content.resize(msg_content.size());
			std::ranges::copy(msg_content, reinterpret_cast<char*>(content.data()));
			header.size = content.size();
			header.message_type = msg_type;
			}

		explicit Message(MsgHeaderType::enum_type msg_type) {
			header.message_type = msg_type;
		}

		// Returns content of message interpreted as a string
		inline std::string ContentAsString() {
			return {reinterpret_cast<const char*>(content.data()), content.size()};
		}

		MsgHeaderType header;
		std::vector<std::byte> content;
	};


	template<typename MsgHeaderType>
	class SocketConnectionBase {
	public:
		SocketConnectionBase(tsQueue<Message<MsgHeaderType>>& incoming_queue, const std::function<void()>& _MessageReceiveCallback = nullptr) : incoming_msg_queue(incoming_queue), MessageReceiveCallback(_MessageReceiveCallback) {
			static_assert(std::derived_from<MsgHeaderType, MessageHeader<typename MsgHeaderType::enum_type>>, "MsgHeaderType must be derived from MessageHeader class");
		}

		tsQueue<Message<MsgHeaderType>>& incoming_msg_queue;

		void OnMessageReceive() const {
			if (MessageReceiveCallback) MessageReceiveCallback();
		}

	private:
		std::function<void()> MessageReceiveCallback = nullptr;
	};


	template<typename MsgHeaderType>
	class SocketConnectionTCP : public SocketConnectionBase<MsgHeaderType> {
	public:
		SocketConnectionTCP(tsQueue<Message<MsgHeaderType>>& incoming_queue, std::function<void()> _MessageReceiveCallback = nullptr) : SocketConnectionBase<MsgHeaderType>(incoming_queue, _MessageReceiveCallback) {
			m_socket = std::make_unique<asio::ip::tcp::socket>(NetworkManager::GetIO());
		};

		bool ConnectTo(const std::string& ipv4, unsigned port) {
			asio::error_code ec;

			m_endpoint = asio::ip::tcp::endpoint(asio::ip::make_address(ipv4), port);

			m_socket->connect(m_endpoint, ec);
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
			std::cout << "TCP socket disconnecting";
#endif
			if (IsConnected()) {
				m_socket->close();
			}
		}

		bool IsConnected() {
			return m_socket->is_open();
		}

		void SendMsg(Message<MsgHeaderType>& msg) {
			asio::post(NetworkManager::GetIO(),
				[this, msg]() mutable {
					bool currently_writing_msg = !m_outgoing_msg_queue.empty();
					m_outgoing_msg_queue.push_front(msg);
					if (!currently_writing_msg) {
						WriteHeader();
					}
				}
			);
		}

		//ASYNC
		void WriteBody() {
			asio::async_write(*m_socket, asio::buffer(m_outgoing_msg_queue.front().content.data(), m_outgoing_msg_queue.front().content.size()),
				[this](std::error_code ec, std::size_t) {
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
			asio::async_write(*m_socket, asio::buffer(&m_outgoing_msg_queue.front().header, sizeof(MsgHeaderType)),
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
					std::cout << "READING TCP";
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

		asio::ip::tcp::endpoint GetEndpoint() {
			return m_endpoint;
		}


	private:
		std::unique_ptr<asio::ip::tcp::socket> m_socket;

		asio::ip::tcp::endpoint m_endpoint;

		tsQueue<Message<MsgHeaderType>> m_outgoing_msg_queue;

		Message<MsgHeaderType> m_temp_receiving_message;
	};

	template<typename MsgHeaderType>
	class SocketConnectionUDP : public SocketConnectionBase<MsgHeaderType> {
	public:
		SocketConnectionUDP(tsQueue<Message<MsgHeaderType>>& incoming_queue, std::function<void()> _MessageReceiveCallback = nullptr) : SocketConnectionBase<MsgHeaderType>(incoming_queue, _MessageReceiveCallback) {
			m_socket = std::make_unique<asio::ip::udp::socket>(NetworkManager::GetIO());
		}

		void Open(short port) {
			m_socket->open(m_endpoint.protocol());
			m_socket->bind(m_endpoint);
		}

		void SetEndpoint(const std::string& ip, unsigned port) {
			m_endpoint = asio::ip::udp::endpoint(asio::ip::make_address(ip), port);
		}

		void SendMsg(Message<MsgHeaderType>& msg) {
			msg.header.body_checksum = CRC::Calculate(msg.content.data(), msg.content.size(), CRC::CRC_32());
			asio::post(NetworkManager::GetIO(),
				[this, msg]() mutable {
					bool currently_writing_msg = !m_outgoing_msg_queue.empty();
#ifdef ONET_DEBUG
					std::cout << "Pushing message to front\n";
#endif
					m_outgoing_msg_queue.push_front(msg);
					if (!currently_writing_msg) {
						WriteHeader();
					}
				}
			);
		}

		//ASYNC
		void WriteBody() {
			m_socket->async_send_to(asio::buffer(m_outgoing_msg_queue.front().content.data(), m_outgoing_msg_queue.front().content.size()), m_endpoint,
				[this](std::error_code ec, std::size_t) {
					if (!ec) {
#ifdef ONET_DEBUG
						std::cout << "Popping outgoing message\n";
#endif
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
			m_socket->async_send_to(asio::buffer(&m_outgoing_msg_queue.front().header, sizeof(MsgHeaderType)), m_endpoint,
				[this](std::error_code ec, std::size_t) {
					if (!ec) {
						if (m_outgoing_msg_queue.front().content.size() > 0) {
							WriteBody();
						}
						else {
#ifdef ONET_DEBUG
							std::cout << "Popping outgoing message\n";
#endif
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
			m_socket->async_receive_from(asio::buffer(m_temp_receiving_message.content.data() + m_current_bytes_read, m_temp_receiving_message.content.size()), m_endpoint,
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						m_current_bytes_read += length;
						if (m_current_bytes_read == m_temp_receiving_message.header.size) {
#ifdef ONET_DEBUG
							std::cout << "Validating body checksum\n";
#endif
							if (m_temp_receiving_message.header.body_checksum == CRC::Calculate(m_temp_receiving_message.content.data(), m_temp_receiving_message.content.size(), CRC::CRC_32())) {
#ifdef ONET_DEBUG
								std::cout << "ReadBody pushing incoming msg\n";
#endif
								this->incoming_msg_queue.push_back(m_temp_receiving_message);
								this->OnMessageReceive();
							}

							m_current_bytes_read = 0;
							ReadHeader();
						}
						else {
							ReadBody();
						}

					}
					else {
						m_current_bytes_read = 0;
						ONET_HANDLE_ERR(ec);
					}
				}
			);

		}

	

		// ASYNC
		// This reads an entire datagram and discards it to clear corrupted packets, then continues waiting for headers as normal
		void DiscardCorruptedBody() {
			m_temp_receiving_message.content.resize(ONET_MAX_DATAGRAM_SIZE_BYTES);

			m_socket->async_receive_from(asio::buffer(m_temp_receiving_message.content.data(), ONET_MAX_DATAGRAM_SIZE_BYTES), m_endpoint,
				[this](std::error_code ec, std::size_t) {
					if (!ec) {
						// Do nothing, discard and go back to waiting for headers.
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
			m_socket->async_receive_from(asio::buffer(&m_temp_receiving_message.header, sizeof(MsgHeaderType)), m_endpoint,
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						m_current_bytes_read += length;

						if (m_current_bytes_read == sizeof(MsgHeaderType)) {
							m_current_bytes_read = 0;
							if (m_temp_receiving_message.header.size > ONET_MAX_DATAGRAM_SIZE_BYTES) {
								std::cout << "UDP corruption detected, discarding message\n" << std::flush;
								DiscardCorruptedBody();
							}
							else {
								if (m_temp_receiving_message.header.size > 0) {
									m_temp_receiving_message.content.resize(m_temp_receiving_message.header.size);
									ReadBody();
								}
								else {
#ifdef ONET_DEBUG
									std::cout << "ReadHeader pushing incoming msg back (empty)\n";
#endif
									this->incoming_msg_queue.push_back(m_temp_receiving_message);
									ReadHeader();
								}
							}

						}
						else {
							ReadHeader();
						}
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

		size_t m_current_bytes_read = 0;

		tsQueue<Message<MsgHeaderType>> m_outgoing_msg_queue;

		Message<MsgHeaderType> m_temp_receiving_message;

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
			std::scoped_lock(incoming_msg_queue.m_queue_mux);
			if (!incoming_msg_queue.empty() && incoming_msg_queue.front().header.message_type == MsgHeaderType::enum_type::HANDSHAKE) {
				auto msg = incoming_msg_queue.pop_front();
				// This is the initial handshake, server gives connection a unique ID, process and discard.
				m_connection_id = *reinterpret_cast<uint64_t*>(msg.content.data());
				std::cout << "ID RECEIVED: " << m_connection_id << "\n";
			}
		}

		tsQueue<Message<MsgHeaderType>> incoming_msg_queue;
		SocketConnectionTCP<MsgHeaderType> connection_tcp{ incoming_msg_queue };
		SocketConnectionUDP<MsgHeaderType> connection_udp{ incoming_msg_queue };
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
			m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(NetworkManager::GetIO(), asio::ip::tcp::endpoint(asio::ip::tcp::v6(), port));
			
			auto tcp_socket = std::make_shared<SocketConnectionTCP<MsgHeaderType>>(incoming_msg_queue);
			
			m_acceptor->async_accept(tcp_socket->GetSocket(), std::bind(&Server::HandleConnectionRequest, this, std::placeholders::_1, tcp_socket));
		}
		
		void OnUpdate() {
			std::scoped_lock l(incoming_msg_queue.m_queue_mux);
			bool pop = false;
			if (!incoming_msg_queue.empty() && incoming_msg_queue.front().header.message_type == MsgHeaderType::enum_type::REQUEST_CLIENTS_INFO) {
				for (auto& connection : connections.GetVector()) {
					if (connection.connection_id == incoming_msg_queue.front().header.connection_id) {
						Message<MsgHeaderType> msg{ std::to_string(connections.size()), MsgHeaderType::enum_type::RESPONSE_CLIENTS_INFO };
						connection.tcp->SendMsg(msg);
						pop = true;
						break;
					}
				}
			}

			if (pop)
				incoming_msg_queue.pop_front();

		}

		
		void HandleConnectionRequest(const asio::error_code& ec, std::shared_ptr<SocketConnectionTCP<MsgHeaderType>> tcp_socket) {
			std::cout << "Connection accepted: " << "\n";

			std::shared_ptr< SocketConnectionUDP<MsgHeaderType>> udp_socket = nullptr;

			auto tcp_endpoint = tcp_socket->GetEndpoint();
			connections.Lock();
			for (auto& connection : connections.GetVector()) {
				if (connection.tcp->GetEndpoint().address() == tcp_endpoint.address()) {
					std::cout << "Existing UDP socket found for address\n";
					udp_socket = std::make_shared<SocketConnectionUDP<MsgHeaderType>>(incoming_msg_queue);
					udp_socket->SetEndpoint(tcp_endpoint.address().to_string(), ONET_UDP_PORT + 1);
					udp_socket->Open(ONET_UDP_PORT + 1);
					udp_socket->ReadHeader();
				}
			}

			if (!udp_socket) {
				udp_socket = std::make_shared<SocketConnectionUDP<MsgHeaderType>>(incoming_msg_queue);
				udp_socket->SetEndpoint(tcp_endpoint.address().to_string(), ONET_UDP_PORT);
				udp_socket->Open(ONET_UDP_PORT);
				udp_socket->ReadHeader();
			}
			ServerConnection<MsgHeaderType> connection(tcp_socket, udp_socket, GenConnectionID());

			tcp_socket->ReadHeader();

			// Send initial handshake message to give connection a unique identifier.
			Message<MsgHeaderType> msg;
			msg.header.message_type = MsgHeaderType::enum_type::HANDSHAKE;
			msg.content.resize(sizeof(uint64_t));
			msg.header.size = msg.content.size();
			std::memcpy(msg.content.data(), &connection.connection_id, sizeof(uint64_t));

			tcp_socket->SendMsg(msg);

			connections.push_back(std::move(connection));

			auto new_socket = std::make_shared<SocketConnectionTCP<MsgHeaderType>>(incoming_msg_queue);

			m_acceptor->async_accept(new_socket->GetSocket(), std::bind(&Server::HandleConnectionRequest, this, std::placeholders::_1, new_socket));

			connections.Unlock();
		}

		void BroadcastMessageTCP(Message<MsgHeaderType>& msg, uint64_t connection_id_to_ignore = 0) {
			std::scoped_lock l(connection_mux);

			for (auto& connection : connections.GetVector()) {
				if (connection.connection_id == connection_id_to_ignore)
					continue;

				connection.tcp->SendMsg(msg);
			}
		}

		void BroadcastMessageUDP(Message<MsgHeaderType>& msg, uint64_t connection_id_to_ignore = 0) {
			std::scoped_lock l(connection_mux);

			for (auto& connection : connections.GetVector()) {
				if (connection.connection_id == connection_id_to_ignore)
					continue;

				connection.udp->SendMsg(msg);
			}

		}

		void CheckConnectionsAlive() {
			std::scoped_lock l(connection_mux);
			auto& vec = connections.GetVector();
			
			for (int i = 0; i < connections.size(); i++) {
				if (!vec[i].tcp->IsConnected()) {
					std::cout << "Erasing connection\n";
					vec[i].udp->Disconnect();
					vec.erase(vec.begin() + i);
					i--;
				}

			}
		}

		tsQueue<Message<MsgHeaderType>> incoming_msg_queue;

		tsVector<ServerConnection<MsgHeaderType>> connections;
	private:

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