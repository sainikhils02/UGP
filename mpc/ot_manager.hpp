#pragma once

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <iostream>
#include <chrono>

#include <coproto/Socket/AsioSocket.h>
#include <coproto/coproto.h>
#include <libOTe/Base/SimplestOT.h>
#include <cryptoTools/Crypto/PRNG.h>
#include <cryptoTools/Common/BitVector.h>

using boost::asio::awaitable;
using boost::asio::use_awaitable;
using boost::asio::steady_timer;
using boost::asio::io_context;

// OTManager: owns a single TCP socket, runs OT tasks sequentially
// on a dedicated worker thread so the Boost.Asio loop stays free.
class OTManager {
public:
    OTManager(boost::asio::ip::tcp::socket sock)
        : cp_(std::move(sock)), running_(true)
    {
        worker_ = std::thread([this] { run(); });
    }

    ~OTManager() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_ = false;
            cv_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
    }

    // Schedule a send-batch of k OTs; returns a future so the caller
    // can wait (or ignore) completion.
    std::future<std::vector<std::array<osuCrypto::block,2>>>
    send_batch(size_t n, std::vector<std::array<osuCrypto::block,2>> msgs) {
        auto prom = std::make_shared<
            std::promise<std::vector<std::array<osuCrypto::block,2>>>>();
        auto fut = prom->get_future();

        enqueue([this, n, prom, msgs = std::move(msgs)]() mutable {
            try {
                osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());
                osuCrypto::SimplestOT ot;
                coproto::sync_wait(ot.send(msgs, prng, cp_));
                std::cout << "[Sender]: msg[0] = " << msgs[0][0]
                          << " msg[1] = " << msgs[0][1] << std::endl;
                std::cout << "[OTManager/Send] OT done\n";
                prom->set_value(std::move(msgs));
            } catch (...) {
                prom->set_exception(std::current_exception());
            }
        });
        return fut;
    }

    // Schedule a recv-batch of k OTs.
    std::future<std::vector<osuCrypto::block>>
    recv_batch(size_t n, osuCrypto::BitVector choices, std::vector<osuCrypto::block> outputs) {
        auto prom = std::make_shared<
            std::promise<std::vector<osuCrypto::block>>>();
        auto fut = prom->get_future();

        enqueue([this, n, prom,
                 choices = std::move(choices),
                 outputs = std::move(outputs)]() mutable {
            try {
                osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());
                osuCrypto::SimplestOT ot;
                coproto::sync_wait(ot.receive(choices, outputs, prng, cp_));
                std::cout << "[Receiver]: choice = " << choices[0]
                          << " msg = " << outputs[0] << std::endl;
                std::cout << "[OTManager/Recv] OT done\n";
                prom->set_value(std::move(outputs));
            } catch (...) {
                prom->set_exception(std::current_exception());
            }
        });
        return fut;
    }

private:
    void enqueue(std::function<void()> task) {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.push(std::move(task));
        cv_.notify_one();
    }

    void run() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return !running_ || !tasks_.empty(); });
                if (!running_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    coproto::AsioSocket cp_;
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    bool running_;
};


template<typename T>
awaitable<T> await_future(io_context& io, std::future<T>& fut) {
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        steady_timer t(io, 5);
        co_await t.async_wait(use_awaitable);
    }
    co_return fut.get();
}