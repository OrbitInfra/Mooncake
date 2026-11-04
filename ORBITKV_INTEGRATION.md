# OrbitKV TENT integration

`orbitkv-integration-v0.3.14` carries OrbitKV's native ownership contracts on
upstream **v0.3.14**, commit `e1ea5d31bb5e46ff2df9f90e7c2fbaf52fcdee8c`
(`e1ea5d3`, released 2026-11-03). OrbitKV still pins
`orbitkv-integration` at `12cbf549b6351c800de0f94a480276d216b9a43b` until a
separate submodule update is reviewed. That existing branch is unchanged.

OrbitKV builds `tent_shared` from an immutable submodule commit. It does not
resolve a moving branch or apply patches during the build. The original base
was v0.3.13.post1 (`719735896c86b56fabec6cf3e825fb2ea640597a`). The table below
uses the actual parent order of the original commits, oldest first.

## Patch disposition

| Original patch | Disposition on v0.3.14 | Remaining delta / upstream replacement |
| --- | --- | --- |
| `e41334378e` — checked TCP copy and CUDA context ownership | Partial | TCP copy-status propagation and device/context selection are upstream in `964b8cf0` (#3476), `bcdde250` (#3973), and `e3e9d70e` (#4643). Retain strict pointer classification, CUDA-device admission, bounded copy counters, and indefinite owner retention after uncertain copy/synchronization or context-restoration failure. Preserve upstream runtime-device **and exact CUcontext** restoration, pinned-host allocation, and synchronization additions. |
| `784e974f9b` — TCP worker ownership before terminal publication | Dropped | Fully covered by `2ccf4cb6c23609aa3cb84176c09da8da9e0417f0` (#4001): worker-owned callback, exception-to-failure conversion, and no task access after terminal publication. Keep newer upstream rate-limited logging. |
| `84a385c34a` — whole-source TCP admission and dispatch drain | Partial | Keep source epochs in buffer metadata and TCP wire descriptors, irreversible admission closure, bounded drain receipts, and dispatch ownership through handler completion. Handler-map locking is already upstream in `8442365f` (#4147). Preserve upstream unknown-handler errors, UB bootstrap, permission/payload checks, progress-worker signature, and pre-send-only WRITE retries (`2042b5b6`, #4006). |
| `8e67571423` — source-grant and TCP READ epoch fencing | Partial | Keep the C ABI epoch methods, request epoch field, merge separation, and TCP epoch checks before use and after retry resolution. Use upstream read-only metadata retry (`c4d63288`) and generation-aware connection-pool eviction (`68f9756d`, #3684), omitting the redundant unconditional pool-clear API. |
| `f877eeb226` — unregister retry and failed MR ownership | Partial | Upstream `a7496800` (#4720) propagates removal errors and tracks private local registrations. Keep the address coordinator, unpublished-retirement owners, publication revision tracking, batch retry boundaries, and failed-MR ownership until successful deregistration. Retain upstream private-registration cleanup, DMA-BUF bookkeeping, cached pre-deregistration address/length, and MADV_DOFORK handling. |
| `a4d25a1bb7` — partial registration rollback ownership | Partial | Upstream `994296cb` (#4002) rolls back duplicate references and `bb6d1126` (#4515) cleans up partial MRs. Keep preallocated retry owners, strict duplicate options, per-range admission, joining all provider work, and persistent cleanup state when rollback fails. Reuse upstream const `tracksLocalBuffer`, location/permission validation, and successful private registrations; retain metrics accounting. |
| `12cbf549b6` — integration documentation | Applied and updated | This document records the new base, dispositions, commands, and consumer requirements. |

The replay commits retain the original provenance trailers. Their historical
messages describe the 2026-10-21 import; the table above describes this port.
The subsequent integration/test commit contains adaptations and regressions
needed to exercise the carried contracts against v0.3.14.
Failed MR cleanup also preserves both manager and context ownership during
device removal and bulk cleanup. Terminal transport/engine teardown fails stop
if quiescence or deregistration cannot be proved; an error never permits those
owners to be discarded. Successful partial-registration rollback remains eager.

## Required upstream fixes checked

All of these commits are ancestors of the selected release and remain present:

| PR | Commit | Integration decision |
| --- | --- | --- |
| #4643 | `e3e9d70eea3c8ea9eb236b4c9c92f361fbbe533f` | Preserve exact context/runtime restoration; keep stronger retention on uncertain completion. |
| #4839 | `b2c7d0a7b5cec0b04563138f40d4c40937dd63fe` | CUDA device-plugin context restoration and its handled-copy-error contract remain unchanged; test the separate plugin path. |
| #4410 | `7a0585567458a4aba6a58115888c54b7fd1a44e1` | CPU initiators can select a peer GPU over TCP; keep capability/selector coverage. |
| #4383 | `1dd37f57ba944f5190dad30db1267c1c5e4a4a6b` | Endpoint/context terminal teardown remains fail-stop. Explicit failed MR deregistration retains its owner for retry. |
| #4797 | `8bfd298880d447137378cf383017f49d8cd4a3aa` | Merged staging requests share one physical submission; retain its regression. |
| #5068 | `5e50b615b3c1d956f103a0191ec3ef5e7eef7da1` | io_uring waits for submitted I/O before releasing a sub-batch; retain its regression. |
| #5132 | `33f5ea0fd9f09b921d2d8585fc398aa703888601` | Use yalantinglibs `45c6463c0afbcc46262e373effa30635be3846a0` and its bundled Asio 1.36.0, with upstream timer-cancellation adaptations. |

## Build and verification

OrbitKV's `crates/orbitkv-mooncake-sys/build.rs` configures the standalone transfer
engine entry point, builds `tent_shared`, and stages `libtent_shared.so`,
`libmooncake_common.so`, and `libasio.so`. Its flags are:

```bash
cmake -S mooncake-transfer-engine -B build-orbitkv -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  '-DCMAKE_BUILD_RPATH=$ORIGIN' '-DCMAKE_INSTALL_RPATH=$ORIGIN' \
  -DBUILD_UNIT_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF \
  -DWITH_RUST_EXAMPLE=OFF -DUSE_HTTP=OFF -DUSE_ETCD=OFF -DUSE_REDIS=OFF \
  -DUSE_TENT=ON -DWITH_METRICS=OFF -DUSE_CUDA=OFF
cmake --build build-orbitkv --target tent_shared --parallel 2
```

`USE_CUDA` follows OrbitKV's feature selection (ON for its default CUDA 13 build).
Ninja is a verification choice, not an OrbitKV requirement. The standalone
CMake entry point now declares the new `Mooncake::cli11` dependency and enables
CTest when tests are requested. Install `libcli11-dev` on Linux along with the
normal TENT dependencies; no new consumer CMake flag is required.

The maintained command builds the production target with unit tests disabled,
then reconfigures the same build with `BUILD_UNIT_TESTS=ON`, builds the selected
tests, and runs CTest with per-case GoogleTest XML and CTest JUnit output:

```bash
USE_CUDA=OFF BUILD_JOBS=2 scripts/ci/run_orbitkv_tent_checks.sh build-orbitkv
USE_CUDA=ON BUILD_JOBS=2 scripts/ci/run_orbitkv_tent_checks.sh build-orbitkv-cuda
```

The CPU selection covers TCP transport, permissions and copy failures, stale
epochs and permanent source closure, dispatch timeout/drain, segment management
and metadata retry, registration rollback and memory lifecycle, RPC reconnect,
RDMA endpoint/context lifecycle and transport injection, io_uring ownership,
request merging, transport selection, runtime dispatch, staging failover, and
progress workers. The CUDA selection builds the shared library and runs the
allocator ownership and upstream plugin fault-injection tests. It also compiles
the upstream real-GPU plugin test without claiming to run it on a GPU-less
runner. `tent_orbitkv_abi_test` checks all 23 consumed C symbols, signatures, and
64-bit struct layouts, and loads the built shared library with `RTLD_NOW`.

[The fork workflow](.github/workflows/orbitkv-integration.yml) runs both Linux
configurations and uploads configuration, build logs, CTest logs, and per-case
XML. If GitHub has not registered the new workflow from a branch push, dispatch
it through the existing workflow ID without changing the default branch:

```bash
gh workflow run ci-on-label.yml --repo OrbitInfra/Mooncake \
  --ref orbitkv-integration-v0.3.14
```

Inspect skipped GoogleTest cases as well as the CTest summary. Hardware
RDMA, physical CUDA copies/context behavior, multi-GPU copies, and installed
OrbitKV qualification remain separate hardware gates. A source-close receipt
only proves whole-source CPU/TCP handler drain, not CUDA/RDMA physical drain or
requester-batch retirement. Timeout or failed cleanup never authorizes reuse.

## Consuming this branch

In a separate OrbitKV session, update the submodule commit and branch metadata,
then update `SOURCE_VERSION` and `SOURCE_REVISION` in `orbitkv-mooncake-sys` to
match the reviewed new pin. Preserve all existing FFI signatures and layouts;
no Rust FFI or CMake flag change is needed. Rebuild all three shared libraries
from the new source together. Do not reuse a yalantinglibs override or build
cache pinned to `7801bc9`; the release fetches the new pin automatically.

Peers must use the matching OrbitKV TENT wire contract. Do not mix this branch
with stock upstream TCP peers, which lack the epoch envelope. Re-run OrbitKV's
native/installed-runtime ownership and hardware gates before accepting its
submodule bump. Do not force-push, rename, or delete the pinned integration
branch. This update does not change OrbitKV or the fork's main branch.
