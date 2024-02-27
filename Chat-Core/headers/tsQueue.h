#pragma once
#include <deque>
#include <mutex>

namespace ONET {
	template<typename T>
	class tsQueue {
	public:
		void push_back(const T& data) {
			std::scoped_lock l(m_queue_mux);
			m_queue.push_back(data);
		}

		void push_front(const T& data) {
			std::scoped_lock l(m_queue_mux);
			m_queue.push_front(data);
		}

		T pop_front() {
			std::scoped_lock l(m_queue_mux);
			T msg = m_queue.front();
			m_queue.pop_front();
			return msg;
		}

		T pop_back() {
			std::scoped_lock l(m_queue_mux);
			T msg = m_queue.back();
			m_queue.pop_back();
			return msg;
		}

		size_t size() {
			std::scoped_lock l(m_queue_mux);
			return m_queue.size();
		}

		T& front() {
			std::scoped_lock l(m_queue_mux);
			return m_queue.front();
		}

		T& back() {
			std::scoped_lock l(m_queue_mux);
			return m_queue.back();
		}

		bool empty() {
			std::scoped_lock l(m_queue_mux);
			return m_queue.empty();
		}

		void Lock() {
			m_queue_mux.lock();
		}

		void Unlock() {
			m_queue_mux.unlock();
		}

		std::deque<T>& GetQueue() {
			return m_queue;
		}

	private:
		std::recursive_mutex m_queue_mux;
		std::deque<T> m_queue;
	};
}