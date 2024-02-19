#pragma once
#include <asio.hpp>
#include <iostream>
#include "tsQueue.h"
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

		static bool Init() {
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

		bool I_Init() {
			asio::io_context::work idle_work{ m_io };
			m_net_thread = std::thread([&]() {while (true) { m_io.run(); }; });
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

	template<typename HeaderType>
	struct Message {
		MessageHeader<HeaderType> header;

		std::vector<std::byte> content;
	};

	template<typename MsgHeaderType>
	class SocketConnection {
	public:
		SocketConnection() {
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
			if (IsConnected())
				m_socket->close();
		}

		bool IsConnected() {
			return m_socket->is_open();
		}

		void SendMsg(const Message<MsgHeaderType>& msg) {
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
			asio::async_write(*m_socket, asio::buffer(&m_outgoing_msg_queue.front().header, sizeof(MessageHeader<MsgHeaderType>)),
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
						m_incoming_msg_queue.push_back(m_temp_receiving_message);
					}
					else {
						ONET_HANDLE_ERR(ec);
						ReadHeader();
					}
				}
			);

		}
		
		// ASYNC
		void ReadHeader() {
			asio::async_read(*m_socket, asio::buffer(&m_temp_receiving_message.header, sizeof(MessageHeader<MsgHeaderType>)),
				[this](std::error_code ec, std::size_t length) {
					if (!ec) {
						if (m_temp_receiving_message.header.size > 0) {
							m_temp_receiving_message.content.resize(m_temp_receiving_message.header.size);
							ReadBody();
						}
						else {
							m_incoming_msg_queue.push_back(m_temp_receiving_message);
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

		tsQueue<Message<MsgHeaderType>> m_incoming_msg_queue;
	private:

		tsQueue<Message<MsgHeaderType>> m_outgoing_msg_queue;

		Message<MsgHeaderType> m_temp_receiving_message;

		std::unique_ptr<asio::ip::tcp::socket> m_socket;
	};

	enum class MessageType {
		PING
	};

	class ClientInterface {
	public:
		void OpenToConnections(unsigned port) {
			m_acceptor = std::make_unique<asio::ip::tcp::acceptor>(NetworkManager::GetIO(), asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));
			m_acceptor->async_accept(connection.GetSocket(), [this](std::error_code ec) {std::cout << "Connection accepted\n"; connection.ReadHeader(); });
		}

		SocketConnection<MessageType> connection;
	private:
		std::unique_ptr<asio::ip::tcp::acceptor> m_acceptor;
	};

}