#pragma once

#include <vector>
#include <deque>
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <stdexcept> 
#include <atomic> // --- NUOVO --- Serve per il contatore atomico

#include "taskFactory.hpp"

class threadPool {
#if !defined(__cpp_lib_move_only_function) || __cpp_lib_move_only_function < 202110
    using Task = std::function<void()>;
#else
    using Task = std::move_only_function<void()>;
#endif

    std::vector<std::jthread> workers_;   // Worker threads owned by the pool
    std::mutex m_;                        // protects shared state
    std::condition_variable cv_;          // used to wake sleeping workers
    bool done_ = false;                   // becomes true when shutdown starts
    std::deque<Task> q_;                  // queue of pending tasks

    // --- NUOVO --- Variabili per la sincronizzazione del wait_all
    std::atomic<size_t> active_tasks_{0}; 
    std::condition_variable wait_cv_;     

    static thread_local int worker_id_;

    void worker_loop(int id) {
        worker_id_ = id;
        for (;;) {
            Task task;  // local task extracted from the shared queue

            {
                std::unique_lock<std::mutex> lk(m_);

                // Wait until:
                // 1. shutdown has been requested, or
                // 2. there is at least one task to execute
                cv_.wait(lk, [&] { return done_ || !q_.empty(); });

                // If shutdown was requested and no work is left, terminate this Worker
                if (done_ && q_.empty()) return;

                // Remove one task from the front of the queue.
                task = std::move(q_.front());
                q_.pop_front();
            }

            // Execute the task outside the critical section.
            task();

            // --- NUOVO --- Decrementa il contatore e sveglia wait_all se abbiamo finito
            if (--active_tasks_ == 0) {
                wait_cv_.notify_all();
            }
        }
    }

public:
    explicit threadPool(unsigned n = std::max(1u, std::thread::hardware_concurrency())) {
        // hardware_concurrency() may return 0 on some systems
        if (n == 0) {
            throw std::invalid_argument("threadPool: n must be > 0");
        }

        workers_.reserve(n);

        // Start n worker threads.
        for (unsigned i = 0; i < n; ++i) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    //
    // The pool is neither copyable nor movable.
    //
    threadPool(const threadPool&)            = delete;
    threadPool& operator=(const threadPool&) = delete;
    threadPool(threadPool&&)                 = delete;
    threadPool& operator=(threadPool&&)      = delete;

    ~threadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;   // signal shutdown
        }

        // Wake all workers so they can either:
        // - finish pending tasks, or
        // - exit if the queue is empty
        cv_.notify_all();

        for (auto& thread : workers_) {
            thread.join();
        }
    }

    template<class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> 
    {
        static_assert(std::is_invocable_v<F, Args...>, "F(Args...) is not callable");

        auto [task, future] = make_task(std::forward<F>(f), std::forward<Args>(args)...);

        // --- NUOVO --- Incrementa il contatore prima di accodare il task
        active_tasks_++; 

        {
            std::lock_guard<std::mutex> lk(m_);

            if (done_) {
                // Se c'è un'eccezione, dobbiamo ri-decrementare per non bloccare eventuali wait_all
                active_tasks_--; 
                throw std::runtime_error("threadPool: submit() called during shutdown");
            }

            q_.emplace_back(std::move(task));
        }

        // Wake one worker to execute the newly submitted task
        cv_.notify_one();

        return future;
    }

    // --- NUOVO --- Metodo per aspettare la fine di tutti i task senza allocare roba
    void wait_all() {
        std::unique_lock<std::mutex> lk(m_);
        wait_cv_.wait(lk, [this] { return active_tasks_ == 0; });
    }

    size_t get_num_threads() const {
        return workers_.size();
    }

    int thread_id() const {
        return worker_id_;
    }
};

thread_local int threadPool::worker_id_ = -1;