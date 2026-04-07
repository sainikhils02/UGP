# mpc

A 3-party MPC prototype with async networking and OT-backed utilities.

## OTManager and Real OTs

The file mpc/ot_manager.hpp contains OTManager, which owns a dedicated OT socket and runs OT work on a worker thread.

### Random OT APIs

- send_batch(n, msgs)
  - Sender provides random pair pads per OT.
- recv_batch(n, choices, outputs)
  - Receiver obtains random selected pad per OT.


### Real OT APIs 

- ot_send(n, msgs)
  - Sender input: for each OT index i, a real pair (m0_i, m1_i).
- ot_receive(n, choices)
  - Receiver input: choice bit c_i for each OT index i.
  - Receiver output: m_{c_i} for each i.

Implementation is layered on top of random OT:

1. Random OT generates random masks r0, r1 at sender and r_q at receiver.
2. Receiver sends one equality bit per OT: eq = (c == q).
3. Sender sends masked pair:
   - if eq = 1: (m0 XOR r0, m1 XOR r1)
   - if eq = 0: (m0 XOR r1, m1 XOR r0)
4. Receiver unmasks selected entry with r_q and recovers m_c.

The benchmark utility is in mpc/ot_tests.cpp.

## Build (whole utility)

Build in this order.

### 1) Build libOTe

From repository root:

```bash
cd libOTe
python build.py --all --boost --sodium
```

### 2) Build mpc and all targets (including ot_tests)

From repository root:

```bash
cmake -S mpc -B mpc/build
cmake --build mpc/build
```

If CMake cannot find libOTe, pass CMAKE_PREFIX_PATH pointing to your libOTe build/install location.

Example:

```bash
cmake -S mpc -B mpc/build -DCMAKE_PREFIX_PATH=/usr/local
cmake --build mpc/build
```

## Run ot_tests

Use two terminals.

Terminal 1 (sender side):

```bash
./mpc/build/ot_tests p0
```

Terminal 2 (receiver side):

```bash
./mpc/build/ot_tests p1
```

## Sample output

Sender side sample:

```text
[P0] Iter 0 first OT pair: (3c882e38df7585382f7ade0a41c380b3, 1bd4ee578e2417db661a014a5f3700be)
[P0] Iter 1 first OT pair: (71f5d342747c0742ae26199374116f94, a420e3da8284737181e6d303c7390d15)
[P0] Iter 2 first OT pair: (08d8b83f844e309789bff3ad715f946a, 85f251960966ff32227bd5fc78ca2eed)
[P0] Iter 3 first OT pair: (bc1ccfbaa01f4dc68cf87c371cd706c8, bb7c3680bfc693eba29d11bd6987ad46)
[P0] Iter 4 first OT pair: (bea123f60e1dbc4a67d7c62665e545ac, bbc1d7230e3abe993d99b328d8c79489)
[P0] OT Benchmark:
Avg: 178374 us
Min: 173599 us
Max: 208014 us
Time per OT: 178374 us
```

Receiver side sample:

```text
[P1] Iter 0 first OT recv: choice=1 value=1bd4ee578e2417db661a014a5f3700be
[P1] Iter 1 first OT recv: choice=1 value=a420e3da8284737181e6d303c7390d15
[P1] Iter 2 first OT recv: choice=0 value=08d8b83f844e309789bff3ad715f946a
[P1] Iter 3 first OT recv: choice=1 value=bb7c3680bfc693eba29d11bd6987ad46
[P1] Iter 4 first OT recv: choice=0 value=bea123f60e1dbc4a67d7c62665e545ac
[P1] OT Benchmark:
Avg: 178332 us
Min: 135734 us
Max: 198616 us
Time per OT: 178332 us
```
## Note:
- Repo layout: `libOTe/` is upstream OT/VOLE/DPF lib (external dep), `mpc/` is a 3-party async MPC prototype that links `oc::libOTe`.
- Build MPC: `cmake -S mpc -B mpc/build && cmake --build mpc/build` (binaries in `mpc/build`: `mpc_async`, `ot_tests`, `mpcops_test`, `test_shares`, `locoram_test`, `dpf_test`).
- OT channel: `OTManager` (`mpc/ot_manager.hpp`) wraps a dedicated socket and runs SimplestOT via coproto on a worker thread; schedule with `send_batch`/`recv_batch`, then `await_future(io, fut)` inside the Asio loop.
- Linking note: `find_package(libOTe REQUIRED COMPONENTS std_20 boost)` then link `oc::libOTe`; if CMake cannot find it, pass `-DCMAKE_PREFIX_PATH=<libOTe build/install>`.
- To run the OT-sender and receiver functions please find the illustrations in `run_p0` and `run_p1` functions in mpc/ot_tests.cpp