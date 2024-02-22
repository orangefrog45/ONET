#pragma once
#include <vector>
#include <mutex>

namespace ONET {
	template<typename T>
	class tsVector {
	public:
		void push_back(const T& data) {
			std::scoped_lock l(m_vec_mux);
			m_vec.push_back(data);
		}

		T pop_back() {
			std::scoped_lock l(m_vec_mux);
			T msg = m_vec.back();
			m_vec.pop_back();
			return msg;
		}

		size_t size() {
			std::scoped_lock l(m_vec_mux);
			return m_vec.size();
		}

		T& front() {
			std::scoped_lock l(m_vec_mux);
			return m_vec.front();
		}

		T& back() {
			std::scoped_lock l(m_vec_mux);
			return m_vec.back();
		}

		bool empty() {
			std::scoped_lock l(m_vec_mux);
			return m_vec.empty();
		}

		void Lock() {
			m_vec_mux.try_lock();
		}

		void Unlock() {
			m_vec_mux.unlock();
		}

		// Not thread-safe unless locks are used with the calling thread
		std::vector<T>& GetVector() {
			return m_vec;
		}

	private:
		std::recursive_mutex m_vec_mux;
		std::vector<T> m_vec;
	};
}