#pragma once
#include <vector>
#include <mutex>

namespace ONET {
	template<typename T>
	class tsVector {
	public:
		void push_back(const T& data) {
			std::scoped_lock l(mux);
			m_vec.push_back(data);
		}

		T pop_back() {
			std::scoped_lock l(mux);
			T msg = m_vec.back();
			m_vec.pop_back();
			return msg;
		}

		size_t size() {
			std::scoped_lock l(mux);
			return m_vec.size();
		}

		T& front() {
			std::scoped_lock l(mux);
			return m_vec.front();
		}

		T& back() {
			std::scoped_lock l(mux);
			return m_vec.back();
		}

		bool empty() {
			std::scoped_lock l(mux);
			return m_vec.empty();
		}

		std::mutex& GetMutex() {
			return mux;
		}

		// Not thread-safe unless locks are used with the calling thread
		std::vector<T>& GetVector() {
			return m_vec;
		}

		std::mutex mux;
	private:
		std::vector<T> m_vec;
	};
}