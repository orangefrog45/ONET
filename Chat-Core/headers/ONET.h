#pragma once
#include <asio.hpp>
#include <string>
#include "tsQueue.h"
#include "tsVector.h"

namespace ONET {
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define CONCAT_IMPL(x, y) x##y
#define FUNC_NAME __FUNCTION__ 
#define ONET_HANDLE_ERR(x) if (auto cb = NetworkManager::GetErrorCallback()) cb(x, __LINE__)

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

		static void SetErrorCallback(std::function<void(asio::error_code, unsigned err_line)> cb) {
			Get().error_callback = cb;
		}

		static bool IsNetThreadRunning() {
			return !Get().m_net_thread.joinable();
		}

	private:
		NetworkManager() = default;

		void I_Init() {
			asio::io_context::work idle_work{ m_io };
			m_net_thread = std::thread([&]() { m_io.run(); });
		}

		bool I_Shutdown() {
			m_io.stop();
			if (m_net_thread.joinable()) m_net_thread.join();
		}

		std::function<void(asio::error_code, unsigned err_line)> error_callback = nullptr;

		asio::io_context m_io;
		std::thread m_net_thread;

	};



	template<typename MsgTypeEnum>
	struct MessageHeader {
		uint64_t size;
		MsgTypeEnum message_type;
	};

	template<typename MsgTypeEnum>
	struct Message {
		Message() = default;
		Message(const std::string& msg_content, MsgTypeEnum msg_type) {
			content.resize(msg_content.size());
			std::ranges::copy(msg_content, reinterpret_cast<char*>(content.data()));
			header.size = content.size();
			header.message_type = msg_type;
			}

		Message(MsgTypeEnum msg_type) {
			header.message_type = msg_type;
		}

		// Returns content of message interpreted as a string
		inline std::string ContentAsString() {
			return {reinterpret_cast<const char*>(content.data()), content.size()};
		}

		MessageHeader<MsgTypeEnum> header;
		std::vector<std::byte> content;
	};

	template<typename MsgTypeEnum>
	class SocketConnection {
	public:
		SocketConnection(tsQueue<Message<MsgTypeEnum>>& incoming_queue) : incoming_msg_queue(incoming_queue) {
			m_socket = std::make_unique<asio::ip::tcp::socket>(NetworkManager::GetIO());
		}

		bool ConnectTo(std::string ipv4, unsigned port) {
			asio::error_code ec;

			asio::ip::tcp::endpoint endpoint(asio::ip::make_address(ipv4), port);

			m_socket->connect(endpoint, ec);
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
			if (IsConnected()) {
				m_socket->close();
			}
		}

		bool IsConnected() {
			return m_socket->is_open();
		}

		void SendMsg(const Message<MsgTypeEnum>& msg) {
			asio::post(NetworkManager::GetIO(),
				[this, msg] {
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
				[this](std::error_code ec, std::size_t length) {
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
			asio::async_write(*m_socket, asio::buffer(&m_outgoing_msg_queue.front().header, sizeof(MessageHeader<MsgTypeEnum>)),
				[this](std::error_code ec, std::size_t length) {
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
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						incoming_msg_queue.push_back(m_temp_receiving_message);
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
			asio::async_read(*m_socket, asio::buffer(&m_temp_receiving_message.header, sizeof(MessageHeader<MsgTypeEnum>)),
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						if (m_temp_receiving_message.header.size > 0) {
							m_temp_receiving_message.content.resize(m_temp_receiving_message.header.size);
							ReadBody();
						}
						else {
							incoming_msg_queue.push_back(m_temp_receiving_message);
							ReadHeader();
						}
					}
					else {
						ONET_HANDLE_ERR(ec);
					}
				}
			);
		}

		auto& GetSocket() {
			return *m_socket;
		}

		tsQueue<Message<MsgTypeEnum>>& incoming_msg_queue;

		std::unique_ptr<asio::ip::tcp::socket> m_socket;
	private:
		tsQueue<Message<MsgTypeEnum>> m_outgoing_msg_queue;

		Message<MsgTypeEnum> m_temp_receiving_message;

	};

	enum class MessageType {
		PING
	};


	template<typename MsgTypeEnum>
	class ClientInterface {
	public:
		void OpenToConnections(unsigned port) {
			m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(NetworkManager::GetIO(), asio::ip::tcp::endpoint(asio::ip::tcp::v6(), port));
			m_acceptor->async_accept(connection.GetSocket(), [this](std::error_code ec) {std::cout << "Connection accepted\n"; connection.ReadHeader(); });
		}

		void Disconnect() {
			if (m_acceptor->is_open())
				m_acceptor->close();

			connection.Disconnect();
		}

		tsQueue<Message<MsgTypeEnum>> incoming_msg_queue;
		SocketConnection<MsgTypeEnum> connection{ incoming_msg_queue };
	private:
		std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;
	};


	template<typename MsgTypeEnum>
	class Server {
	public:
		Server() = default;

		void OpenToConnections(unsigned port) {
			m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(NetworkManager::GetIO(), asio::ip::tcp::endpoint(asio::ip::tcp::v6(), port));
			auto socket = std::make_shared<SocketConnection<MsgTypeEnum>>(incoming_msg_queue);

			m_acceptor->async_accept(*socket->m_socket, std::bind(&Server::HandleConnectionRequest, this, std::placeholders::_1, socket));
		
		}

		void HandleConnectionRequest(const asio::error_code& ec, std::shared_ptr<SocketConnection<MsgTypeEnum>> socket) {
			std::cout << "Connection accepted\n";
			connections.push_back(socket);
			socket->ReadHeader();

			auto new_socket = std::make_shared<SocketConnection<MsgTypeEnum>>(incoming_msg_queue);

			//m_acceptor->async_accept(*socket->m_socket, std::bind(&Server::HandleConnectionRequest, this, std::placeholders::_1, new_socket));

		}

		void BroadcastMessage(const Message<MsgTypeEnum> msg, std::shared_ptr<SocketConnection<MsgTypeEnum>> connection_to_ignore = nullptr) {
			std::scoped_lock l(connection_mux);

			for (auto& connection : connections.GetVector()) {
				if (connection == connection_to_ignore)
					continue;

				connection->SendMsg(msg);
			}

		}

		void CheckConnectionsAlive() {
			std::scoped_lock l(connection_mux);
			const auto& vec = connections.GetVector();

			for (int i = 0; i < connections.size(); i++) {
				if (!vec[i]->IsConnected()) {
					vec.erase(vec.begin() + i);
					i--;
				}

			}
		}

		tsQueue<Message<MsgTypeEnum>> incoming_msg_queue;

		tsVector<std::shared_ptr<SocketConnection<MsgTypeEnum>>> connections;
	private:
		std::mutex connection_mux;

		std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;

	};
}