// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lterm {

// Minimal persistent pool: parallelFor splits [0,n) into one contiguous chunk per thread.
class ThreadPool {
public:
    explicit ThreadPool(int threads);
    ~ThreadPool();
    int size() const { return threads_; }
    void parallelFor(int n, const std::function<void(int, int)>& fn);

private:
    void worker(int index);
    int threads_;
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    const std::function<void(int, int)>* job_ = nullptr;
    int n_ = 0;
    unsigned generation_ = 0;
    int pending_ = 0;
    bool quit_ = false;
};

}  // namespace lterm
