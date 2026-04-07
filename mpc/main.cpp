#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <iostream>

#include "network.hpp"
#include "mpcops.hpp"
#include "shares.hpp"

using namespace boost::asio;
using namespace boost::asio::experimental::awaitable_operators;
using namespace std::chrono_literals;
using ip::tcp;

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
        std::cerr << "Usage: ./mpc_async <p0|p1|p2>\n";
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
