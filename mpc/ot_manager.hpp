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
#include <array>
#include <vector>
#include <cstdint>
#include <stdexcept>

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

        try {
            coproto::sync_wait(cp_.flush());
        } catch (...) {
            // Swallow flush errors during teardown so destructor remains noexcept-safe.
        }
    }

    // Schedule a send-batch of k random OTs; returns a future so the caller
    // can wait (or ignore) completion.
    std::future<std::vector<std::array<osuCrypto::block, 2>>>
    send_batch(size_t n, std::vector<std::array<osuCrypto::block, 2>> msgs) {
        auto prom = std::make_shared<
            std::promise<std::vector<std::array<osuCrypto::block, 2>>>>();
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

    // Schedule a recv-batch of k random OTs.
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

    // Real/message OT sender API built on top of random OT.
    // Sender inputs (m0, m1) for each OT and receiver gets m_c.
    std::future<std::vector<std::array<osuCrypto::block, 2>>>
    ot_send(size_t n, std::vector<std::array<osuCrypto::block, 2>> msgs) {
        auto prom = std::make_shared<
            std::promise<std::vector<std::array<osuCrypto::block, 2>>>>();
        auto fut = prom->get_future();

        enqueue([this, n, prom, msgs = std::move(msgs)]() mutable {
            try {
                if (msgs.size() != n)
                    throw std::runtime_error("ot_send: msgs.size() must equal n");

                if (n == 0) {
                    prom->set_value(std::move(msgs));
                    return;
                }

                osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());
                osuCrypto::SimplestOT ot;

                // Step 1: run random OT where sender has random pads (r0, r1).
                std::vector<std::array<osuCrypto::block, 2>> randomPads(n);
                for (size_t i = 0; i < n; ++i) {
                    randomPads[i][0] = prng.get<osuCrypto::block>();
                    randomPads[i][1] = prng.get<osuCrypto::block>();
                }
                coproto::sync_wait(ot.send(randomPads, prng, cp_));

                // Step 2: receiver sends per-OT eq bit: eq = (c == q).
                std::vector<std::uint8_t> eqBytes((n + 7) / 8, 0);
                coproto::sync_wait(cp_.recv(eqBytes));

                // Step 3: send masked payload pair using eq branch.
                std::vector<osuCrypto::block> masked(2 * n);
                for (size_t i = 0; i < n; ++i) {
                    const bool eq = ((eqBytes[i >> 3] >> (i & 7)) & 0x1) != 0;

                    // eq=1  => (m0^r0, m1^r1)
                    // eq=0  => (m0^r1, m1^r0)
                    masked[2 * i + 0] = msgs[i][0] ^ randomPads[i][eq ? 0 : 1];
                    masked[2 * i + 1] = msgs[i][1] ^ randomPads[i][eq ? 1 : 0];
                }
                coproto::sync_wait(cp_.send(std::move(masked)));
                coproto::sync_wait(cp_.flush());

                prom->set_value(std::move(msgs));
            } catch (...) {
                prom->set_exception(std::current_exception());
            }
        });

        return fut;
    }

    // Real/message OT receiver API built on top of random OT.
    // Receiver inputs choice bits c and gets output m_c for each OT.
    std::future<std::vector<osuCrypto::block>>
    ot_receive(size_t n, osuCrypto::BitVector choices) {
        auto prom = std::make_shared<std::promise<std::vector<osuCrypto::block>>>();
        auto fut = prom->get_future();

        enqueue([this, n, prom, choices = std::move(choices)]() mutable {
            try {
                if (choices.size() != n)
                    throw std::runtime_error("ot_receive: choices.size() must equal n");

                if (n == 0) {
                    prom->set_value({});
                    return;
                }

                osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());
                osuCrypto::SimplestOT ot;

                // Step 1: run random OT with random receiver choice q.
                osuCrypto::BitVector q(n);
                q.randomize(prng);

                std::vector<osuCrypto::block> r_q(n);
                coproto::sync_wait(ot.receive(q, r_q, prng, cp_));

                // Step 2: send eq bits where eq = (c == q).
                std::vector<std::uint8_t> eqBytes((n + 7) / 8, 0);
                for (size_t i = 0; i < n; ++i) {
                    const bool c = static_cast<bool>(choices[i]);
                    const bool qb = static_cast<bool>(q[i]);
                    const bool eq = (c == qb);
                    if (eq)
                        eqBytes[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
                }
                coproto::sync_wait(cp_.send(std::move(eqBytes)));
                coproto::sync_wait(cp_.flush());

                // Step 3: receive masked pairs and recover selected messages.
                std::vector<osuCrypto::block> masked(2 * n);
                coproto::sync_wait(cp_.recv(masked));

                std::vector<osuCrypto::block> outputs(n);
                for (size_t i = 0; i < n; ++i) {
                    const bool c = static_cast<bool>(choices[i]);
                    outputs[i] = masked[2 * i + (c ? 1 : 0)] ^ r_q[i];
                }

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
