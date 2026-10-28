// SPDX-License-Identifier: GPL-3.0-or-later
#include "thread_pool.hpp"

namespace lterm {

ThreadPool::ThreadPool(int threads) : threads_(threads < 1 ? 1 : threads) {
    for (int i = 1; i < threads_; ++i) workers_.emplace_back(&ThreadPool::worker, this, i);
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

static inline void chunk(int n, int parts, int i, int& a, int& b) {
    a = static_cast<int>(static_cast<long long>(n) * i / parts);
    b = static_cast<int>(static_cast<long long>(n) * (i + 1) / parts);
}

void ThreadPool::parallelFor(int n, const std::function<void(int, int)>& fn) {
    if (threads_ == 1) {
        fn(0, n);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        job_ = &fn;
        n_ = n;
        pending_ = threads_ - 1;
        ++generation_;
    }
    cv_.notify_all();
    int a, b;
    chunk(n, threads_, 0, a, b);
    fn(a, b);
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [this] { return pending_ == 0; });
    job_ = nullptr;
}

void ThreadPool::worker(int index) {
    unsigned seen = 0;
    for (;;) {
        const std::function<void(int, int)>* job;
        int n;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
            job = job_;
            n = n_;
        }
        int a, b;
        chunk(n, threads_, index, a, b);
        (*job)(a, b);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (--pending_ == 0) done_.notify_one();
        }
    }
}

}  // namespace lterm
