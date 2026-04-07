#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <iostream>
#include <chrono>
#include <vector>
#include <array>
#include <limits>

#include "network.hpp"
#include "ot_manager.hpp"

using namespace boost::asio;
using namespace boost::asio::experimental::awaitable_operators;
using ip::tcp;

const int NUM_ITERS = 1000;
const int WARMUP = 5;
const int DEBUG_PRINT_ITERS = 5;
const int SIZE = 1;

awaitable<tcp::socket> accept_on(io_context& io, uint16_t port) {
    tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), port));
    tcp::socket sock = co_await acc.async_accept(use_awaitable);
    co_return sock;
}

awaitable<void> run_p0(io_context& io) {
    tcp::socket ot_sock = co_await accept_on(io, 12000);
    OTManager ot(std::move(ot_sock));

    std::vector<long long> timings;
    osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());

    for (int iter = 0; iter < NUM_ITERS; ++iter) {
        std::vector<std::array<osuCrypto::block, 2>> msgs(SIZE);
        for (auto& m : msgs) {
            m[0] = prng.get<osuCrypto::block>();
            m[1] = prng.get<osuCrypto::block>();
        }

        if (iter < DEBUG_PRINT_ITERS) {
            std::cout << "[P0] Iter " << iter
                      << " first OT pair: (" << msgs[0][0]
                      << ", " << msgs[0][1] << ")\n";
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto fut = ot.ot_send(SIZE, msgs);
        co_await await_future(io, fut);
        auto end = std::chrono::high_resolution_clock::now();

        long long us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        if (iter >= WARMUP) timings.push_back(us);
    }

    long long sum = 0;
    long long mn = std::numeric_limits<long long>::max();
    long long mx = 0;
    for (auto t : timings) {
        sum += t;
        mn = std::min(mn, t);
        mx = std::max(mx, t);
    }

    std::cout << "[P0] OT Benchmark:\n";
    std::cout << "Avg: " << (sum / timings.size()) << " us\n";
    std::cout << "Min: " << mn << " us\n";
    std::cout << "Max: " << mx << " us\n";
    std::cout << "Time per OT: " << (sum / timings.size()) / SIZE << " us\n";

    co_return;
}

awaitable<void> run_p1(io_context& io) {
    tcp::socket ot_sock = co_await connect_with_retry(io, "127.0.0.1", 12000);
    OTManager ot(std::move(ot_sock));

    std::vector<long long> timings;
    osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());

    for (int iter = 0; iter < NUM_ITERS; ++iter) {
        osuCrypto::BitVector choices(SIZE);
        choices.randomize(prng);

        auto start = std::chrono::high_resolution_clock::now();
        auto fut = ot.ot_receive(SIZE, choices);
        auto outputs = co_await await_future(io, fut);
        auto end = std::chrono::high_resolution_clock::now();

        if (iter < DEBUG_PRINT_ITERS) {
            std::cout << "[P1] Iter " << iter
                      << " first OT recv: choice=" << static_cast<int>(static_cast<bool>(choices[0]))
                      << " value=" << outputs[0] << "\n";
        }

        long long us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        if (iter >= WARMUP) timings.push_back(us);
    }

    long long sum = 0;
    long long mn = std::numeric_limits<long long>::max();
    long long mx = 0;
    for (auto t : timings) {
        sum += t;
        mn = std::min(mn, t);
        mx = std::max(mx, t);
    }

    std::cout << "[P1] OT Benchmark:\n";
    std::cout << "Avg: " << (sum / timings.size()) << " us\n";
    std::cout << "Min: " << mn << " us\n";
    std::cout << "Max: " << mx << " us\n";
    std::cout << "Time per OT: " << (sum / timings.size()) / SIZE << " us\n";

    co_return;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./ot_tests <p0|p1>\n";
        return 1;
    }

    io_context io;
    std::string role = argv[1];

    if (role == "p0") {
        co_spawn(io, run_p0(io), detached);
    } else if (role == "p1") {
        co_spawn(io, run_p1(io), detached);
    } else {
        std::cerr << "Invalid role for ot_tests. Use p0 or p1.\n";
        return 1;
    }

    io.run();
    return 0;
}
