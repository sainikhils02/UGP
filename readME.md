- Repo layout: `libOTe/` is upstream OT/VOLE/DPF lib (external dep), `mpc/` is a 3-party async MPC prototype that links `oc::libOTe`.
- Build libOTe: `cd libOTe && python build.py --all --boost --sodium`; set `CMAKE_PREFIX_PATH` to the libOTe build/install root if not installed.
- Build MPC: `cmake -S mpc -B mpc/build && cmake --build mpc/build` (binaries in `mpc/build`: `mpc_async`, `mpcops_test`, `test_shares`, `locoram_test`, `dpf_test`).
- OT channel: `OTManager` (`mpc/ot_manager.hpp`) wraps a dedicated socket and runs SimplestOT via coproto on a worker thread; schedule with `send_batch`/`recv_batch`, then `await_future(io, fut)` inside the Asio loop.
- Linking note: `find_package(libOTe REQUIRED COMPONENTS std_20 boost)` then link `oc::libOTe`; if CMake cannot find it, pass `-DCMAKE_PREFIX_PATH=<libOTe build/install>`.
- To run the OT-sender and receiver functions please find the illustrations in `run_p0` and `run_p1` functions in mpc/main.cpp

# OTManager (OT helper) - mpc/ot_manager.hpp

## Overview

`OTManager` encapsulates a single OT TCP socket and runs OT tasks
sequentially on a dedicated worker thread.

- Accepts one `boost::asio::ip::tcp::socket` on construction.
- Uses internal queue + condition variable.
- Worker thread executes one OT job at a time.
- Supports send batch and receive batch.
- Clean shutdown in destructor.

---

## API

### `OTManager(boost::asio::ip::tcp::socket sock)`

- Initialize manager with a connected OT socket.
- Start background worker thread.

### `~OTManager()`

- Signal stop.
- Join worker thread.

### `std::future<std::vector<std::array<osuCrypto::block,2>>> send_batch(size_t n, std::vector<std::array<osuCrypto::block,2>> msgs)`

- Sender side.
- `n` = number of OTs (passed but not enforced in implementation).
- `msgs` = length `n`; each element is `[msg0,msg1]`.
- Internally:
  - `osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());`
  - `osuCrypto::SimplestOT ot;`
  - `coproto::sync_wait(ot.send(msgs, prng, cp_));`
- Returns future-of-msgs (for completion & optional sanity).

### `std::future<std::vector<osuCrypto::block>> recv_batch(size_t n, osuCrypto::BitVector choices, std::vector<osuCrypto::block> outputs)`

- Receiver side.
- `n` = number of OTs (passed but not enforced).
- `choices` = bitvector length `n`; each bit selects msg0/msg1.
- `outputs` = placeholder vector length `n`.
- Internally:
  - `osuCrypto::PRNG prng(osuCrypto::sysRandomSeed());`
  - `osuCrypto::SimplestOT ot;`
  - `coproto::sync_wait(ot.receive(choices, outputs, prng, cp_));`
- Returns future-of-outputs.

---

## Coroutine integration helper

### `awaitable<T> await_future(io_context& io, std::future<T>& fut)`

- Polls `fut` with non-blocking wait.
- Awaits a small `steady_timer` loop.
- Returns `fut.get()` when ready.
- Used in `main.cpp`:
  - `co_await await_future(io, fut1);`

---

## Example usage (mpc/main.cpp)

### P0 (sender)
1. Accept OT socket:
   - `tcp::socket ot_sock = co_await accept_on(io, 12000);`
   - `OTManager ot(std::move(ot_sock));`
2. Prepare messages:
   - `vector<array<block,2>> msgs(128);`
   - random fill.
3. Send:
   - `auto fut1 = ot.send_batch(128, msgs);`
   - `co_await await_future(io, fut1);`

### P1 (receiver)
1. Connect to OT socket:
   - `tcp::socket ot_sock = co_await connect_with_retry(io, "127.0.0.1", 12000);`
   - `OTManager ot(std::move(ot_sock));`
2. Prepare:
   - `BitVector choices(128); choices.randomize(prng);`
   - `vector<block> outputs(128);`
3. Receive:
   - `auto fut1 = ot.recv_batch(128, choices, outputs);`
   - `outputs = co_await await_future(io, fut1);`

---

## Run order

1. `./mpcops_test p2`
2. `./mpcops_test p0`
3. `./mpcops_test p1`

P2 gives prep material; P0/P1 perform OT + mpc_* operations.