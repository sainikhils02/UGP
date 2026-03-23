#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <iostream>
#include <chrono>
#include <random>
#include <thread>

#include "network.hpp"
#include "mpcops.hpp"
#include "shares.hpp"
#include "ot_manager.hpp"

using namespace boost::asio;
using namespace boost::asio::experimental::awaitable_operators;
using namespace std::chrono_literals;
using ip::tcp;

// -----------------------------------------
// arc4random_buf fallback
// -----------------------------------------
// #if !defined(__APPLE__) && (!defined(__GLIBC__) || __GLIBC__ < 2 || __GLIBC_MINOR__ < 36)
// inline void arc4random_buf(void* buf, size_t n) {
//     std::random_device rd;
//     uint8_t* p = static_cast<uint8_t*>(buf);
//     for (size_t i = 0; i < n; ++i)
//         p[i] = static_cast<uint8_t>(rd());
// }
// #endif

// // -----------------------------------------
// // Connection helpers
// // -----------------------------------------
// awaitable<tcp::socket> connect_with_retry(io_context& io, const std::string& host, uint16_t port) {
//     tcp::resolver resolver(io);
//     tcp::socket sock(io);
//     while (true) {
//         try {
//             auto endpoints = co_await resolver.async_resolve(host, std::to_string(port), use_awaitable);
//             co_await async_connect(sock, endpoints, use_awaitable);
//             co_return sock;
//         } catch (...) {
//             steady_timer t(io, 200ms);
//             co_await t.async_wait(use_awaitable);
//         }
//     }
// }

awaitable<tcp::socket> accept_on(io_context& io, uint16_t port) {
    tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), port));
    tcp::socket sock = co_await acc.async_accept(use_awaitable);
    co_return sock;
}

// -----------------------------------------
// P2 — helper NetPeer
// -----------------------------------------
awaitable<void> run_p2(io_context& io) {
    tcp::acceptor acc0(io, tcp::endpoint(tcp::v4(), 9000));
    tcp::acceptor acc1(io, tcp::endpoint(tcp::v4(), 9001));

    std::cout << "[P2] Waiting for P0(:9000) and P1(:9001)...\n";

    tcp::socket sock0 = co_await acc0.async_accept(use_awaitable);
    tcp::socket sock1 = co_await acc1.async_accept(use_awaitable);

    NetPeer p0(Role::P0, std::move(sock0));
    NetPeer p1(Role::P1, std::move(sock1));
    std::cout << "[P2] Both parties connected.\n";

    MPCContext ctx(Role::P2, p0, &p0, &p1);

    // Generate correlated randomness once
    AShare<uint64_t> dummy_x(0, &ctx), dummy_y(0, &ctx);
    co_await mpc_mul(dummy_x, dummy_y, Role::P2, p0, &p0, &p1);
    std::cout << "[P2] Sent triples for MUL.\n";

    XShare<uint64_t> xx{0, &ctx}, yy{0, &ctx};
    co_await mpc_and(xx, yy, Role::P2, p0, &p0, &p1);
    std::cout << "[P2] Sent triples for AND.\n";

    co_return;
}

// -----------------------------------------
// P0 — first computation NetPeer
// -----------------------------------------
awaitable<void> run_p0(io_context& io) {
    tcp::socket sock_p2 = co_await connect_with_retry(io, "127.0.0.1", 9000);
    NetPeer self(Role::P0, std::move(sock_p2));

    tcp::socket peer_sock = co_await accept_on(io, 9100);
    NetPeer peer(Role::P1, std::move(peer_sock));

    tcp::socket ot_sock = co_await accept_on(io, 12000);
    OTManager ot(std::move(ot_sock));

    // Phase 1: run 2 OTs
    osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());
    std::vector<std::array<osuCrypto::block,2>> msgs(128);
    for (auto& m : msgs) {
        m[0] = prng.get<osuCrypto::block>();
        m[1] = prng.get<osuCrypto::block>();
    }
    auto fut1 = ot.send_batch(128, msgs);
    co_await await_future(io, fut1);

    std::vector<std::array<osuCrypto::block,2>> msgs2(128);
    for (auto& m : msgs2) {
        m[0] = prng.get<osuCrypto::block>();
        m[1] = prng.get<osuCrypto::block>();
    }
    auto fut2 = ot.send_batch(128, msgs2);
    co_await await_future(io, fut2);

    MPCContext ctx(Role::P0, self, &peer, nullptr);

    AShare<uint64_t> a(5, &ctx), b(7, &ctx);
    std::cout << "[P0] Starting mpc_mul...\n";
    uint64_t z0 = co_await mpc_mul(a, b, Role::P0, self, &peer);
    std::cout << "[P0] Share z0 = " << z0 << "\n";

    XShare<uint64_t> x1{1, &ctx}, y1{1, &ctx};
    std::cout << "[P0] Starting mpc_and...\n";
    uint64_t z_and0 = co_await mpc_and(x1, y1, Role::P0, self, &peer);
    std::cout << "[P0] Share z_and0 = " << z_and0 << "\n";

    co_return;
}

// -----------------------------------------
// P1 — second computation NetPeer
// -----------------------------------------
awaitable<void> run_p1(io_context& io) {
    tcp::socket sock_p2 = co_await connect_with_retry(io, "127.0.0.1", 9001);
    NetPeer self(Role::P1, std::move(sock_p2));

    tcp::socket peer_sock = co_await connect_with_retry(io, "127.0.0.1", 9100);
    NetPeer peer(Role::P0, std::move(peer_sock));

    tcp::socket ot_sock = co_await connect_with_retry(io, "127.0.0.1", 12000);
    OTManager ot(std::move(ot_sock));

    // Phase 1: match P0's 2 sends
    osuCrypto::BitVector choices(128);
    osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());
    choices.randomize(prng);
    std::vector<osuCrypto::block> outputs(128);
    auto fut1 = ot.recv_batch(128, choices, outputs);
    co_await await_future(io, fut1);

    osuCrypto::BitVector choices2(128);
    choices.randomize(prng);
    std::vector<osuCrypto::block> outputs2(128);
    auto fut2 = ot.recv_batch(128, choices2, outputs2);
    co_await await_future(io, fut2);

    MPCContext ctx(Role::P1, self, &peer, nullptr);

    AShare<uint64_t> a(5, &ctx), b(7, &ctx);
    std::cout << "[P1] Starting mpc_mul...\n";
    uint64_t z1 = co_await mpc_mul(a, b, Role::P1, self, &peer);
    std::cout << "[P1] Share z1 = " << z1 << "\n";

    XShare<uint64_t> x1{1, &ctx}, y1{1, &ctx};
    std::cout << "[P1] Starting mpc_and...\n";
    uint64_t z_and1 = co_await mpc_and(x1, y1, Role::P1, self, &peer);
    std::cout << "[P1] Share z_and1 = " << z_and1 << "\n";

    co_return;
}

// -----------------------------------------
// main()
// -----------------------------------------
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./mpcops_test <p0|p1|p2>\n";
        return 1;
    }

    io_context io;
    std::string role = argv[1];

    if (role == "p0")
        co_spawn(io, run_p0(io), detached);
    else if (role == "p1")
        co_spawn(io, run_p1(io), detached);
    else if (role == "p2")
        co_spawn(io, run_p2(io), detached);
    else {
        std::cerr << "Invalid role\n";
        return 1;
    }

    io.run();
    return 0;
}
