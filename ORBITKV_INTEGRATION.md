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
| `8e67571423` — source-grant and TCP READ epoch fencing | Partial | Keep the C ABI epoch methods, request epoch field, merge separation, and TCP epoch checks before use and after retry resolution. Retain generation-aware connection-pool eviction (`68f9756d`, #3684). The later stale-connection fix below replaces upstream metadata replay (`c4d63288`) with a single pre-send retry at the RPC owner. |
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
`libmooncake_common.so`, and `libasio.so`. v0.3.14 also requires
`tent/src/metrics/libtent_metrics.so` because upstream `904b8ab9` (#3797) made
the metrics singleton shared, including when metrics are disabled. Its flags are:

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
then stages all four libraries, sets `$ORIGIN` runpaths with `patchelf`, and
checks that the relocated library loads without relying on build-directory
runpaths. It then reconfigures with `BUILD_UNIT_TESTS=ON`, builds the selected
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

[The fork workflow](.github/workflows/orbitkv-integration.yml) defines both Linux
configurations and artifact uploads for configuration, build logs, CTest logs,
and per-case XML. After the maintainer enabled fork workflows,
[x86_64 run 41764539451](https://github.com/OrbitInfra/Mooncake/actions/runs/41764539451)
passed both configurations at `a6caba7e`: CUDA OFF passed 19/19 CTest invocations
(451 GoogleTest cases passed, two RDMA cases skipped); CUDA ON passed 3/3 CTest
invocations (14 injected/ABI cases passed). Inspect skipped GoogleTest cases as
well as the CTest summary. That baseline run did not establish repeated-suite
reliability; the stale-connection fix and stress evidence are recorded below. Hardware
RDMA, physical CUDA copies/context behavior, multi-GPU copies, and installed
OrbitKV qualification remain separate hardware gates. A source-close receipt
only proves whole-source CPU/TCP handler drain, not CUDA/RDMA physical drain or
requester-batch retirement. Timeout or failed cleanup never authorizes reuse.

### Observed validation

Linux container: Debian trixie, aarch64, Linux 6.12.76-linuxkit, GCC 14.4.0,
CMake 3.31.6. CUDA validation used NVIDIA's SHA-256-verified CUDA 13.0.2
redistributions (nvcc 13.0.88, cudart 13.0.96) and driver stubs.

| Check | Observed result |
| --- | --- |
| CPU production `tent_shared`, exact OrbitKV flags | Built; relocated four-library runtime loaded. |
| Selected CPU tests | 19/19 CTest invocations passed. Three RDMA availability/hardware cases skipped. Docker's default syscall filter also skipped 20 io_uring cases, all rerun below. |
| io_uring with `--security-opt seccomp=unconfined` | 24/24 GoogleTest cases passed, including in-flight I/O drain and failed-wait cleanup. |
| CUDA production `tent_shared`, exact flags with `USE_CUDA=ON` | Built; relocated four-library runtime loaded. |
| CUDA allocator ownership, upstream plugin context mocks, C ABI | 3/3 CTest invocations, 14/14 GoogleTest cases passed. |
| `tent_cuda_plugin_context_gpu_test` | Built, not run: no physical GPU. |
| macOS native configuration | Stopped at missing CLI11 development headers; no native macOS TENT binary built. |
| Scoped pre-commit | Applicable checks passed. |

Physical RDMA, CUDA device copies/context behavior, and NVLink/multi-GPU execution
were not tested. GDS was not built because cuFile was
absent. The full repository suite and OrbitKV's installed-runtime/Rust tests
were not run; this session qualifies the selected TENT paths and leaves the
OrbitKV pin unchanged. The old three-library runtime was also checked and
failed to load with `libtent_metrics.so` missing, confirming the packaging
change below.

### Fixed in this branch: stale pooled TCP connections

Before the fix, the full `tent_tcp_regression_test --gtest_repeat=200` loop
reproduced a failure on both the fork at `a6caba7e84b51ee799f3a6c38b09d9b2981edf41` and
unmodified upstream v0.3.14 at `e1ea5d31bb5e46ff2df9f90e7c2fbaf52fcdee8c`.
Both completed all 200 repetitions in the same Debian trixie/aarch64 Linux
container described above, with `--security-opt seccomp=unconfined`, GCC 14.4.0,
CUDA OFF, and yalantinglibs `45c6463c0afbcc46262e373effa30635be3846a0`.
Neither baseline used a test filter, assertion change, added retry, or early break.

| Revision | Cases per repetition | Passed case invocations | Failed case invocations | Payload test failures | Permission test failures |
| --- | --- | --- | --- | --- | --- |
| OrbitKV `a6caba7e` | 9 | 1759/1800 | 41 | 21; first at repetition 48 | 20; first at repetition 37 |
| Upstream `e1ea5d31` | 6 | 1163/1200 | 37 | 19; first at repetition 12 | 18; first at repetition 95 |

The originally observed failure is
`TcpRegression.PayloadCapsRejectBeforeAllocationOrDataRpc`:
`ASSERT_TRUE(ControlClient::sendData(address, 0, data.data(), 1).ok())`
at upstream `tcp_regression_test.cpp:289` returns false. The fork's equivalent
assertion at line 299 also passes `sourceEpoch()`. The other failing case is
`TcpRegression.ServerChecksEveryRegistrationPermission`: the error text lacks
`permission denied` (upstream line 260, fork line 270).

`CoroRpcAgent::start()` chooses automatic ports from 15000 through 16999.
The main thread's `ControlClient` RPC agent and its address-keyed idle connection
pools outlive each test server. Reusing a stopped server's address can therefore
select an old connection. A separate two-server diagnostic, linked independently
against each revision without changing either tree, returned `OK` for the first
one-byte send, then `RpcServiceError: ... message: End of file` for the first send
after restarting on the same port. The replacement handler's call count remained
zero on both revisions. The old RPC path evicted the stale pool only after that failed call. This
established the upstream cause independently of the OrbitKV patches. The fork
now fixes it as an OrbitKV patch, with the following ownership rules.

`ControlClient` owns a thread-local `CoroRpcAgent`; the agent leases an exclusive
`coro_rpc_client` from its address-keyed pool. yalantinglibs stops receiving
when no replies are pending, so its `has_closed()` flag alone cannot discover
an idle FIN. The stale client used to reach `call_for()` and report EOF/reset
while writing the request or reading its response. TCP `sendData` and `recvData`,
metadata queries, and other control RPCs all use this owner.

Before dispatch, the owner now performs a nonblocking `MSG_PEEK` on the socket
executor. FIN, reset, other socket errors, or unexpected pending frame bytes
retire the client and its pool generation. A stale socket or failed connect
permits at most one fresh connection attempt, under the same absolute deadline.
The default RPC budget is 30 seconds; TCP data RPCs also honor an earlier
`Request::deadline_ns`. Pool admission is bounded to 64 addresses per agent and
16 active plus idle connections per address. Inactive address pools may be
evicted; active owners retain their pool and budget until completion.

| Caller observation | Behavior |
| --- | --- |
| Known-dead socket before dispatch | Close/evict it and reconnect once; the request and epoch envelope are unchanged. |
| Failed connects or expired deadline before dispatch | `RpcConnectionError` (134); the request was not sent. |
| EOF, reset, EPIPE, timeout, short/invalid frame, or handler exception after dispatch starts | `RpcUnknownEffect` (135); the request may have reached the peer. Close the connection and never replay it automatically. Transport failures also flush its idle pool generation. |
| Successful reply rejecting an old epoch or permissions | Preserve the application `RpcServiceError` (132), including its original rejection text. |
| Active pool capacity exhausted | `TooManyRequests` (2), without sending. |

The liveness check is an optimization, not a delivery proof after dispatch.
yalantinglibs does not expose write-byte counts, so even an EPIPE after
`call_for()` begins is conservatively unknown. Both READ and WRITE follow this
rule. `RpcUnknownEffect` remains in the RPC-service-error family for existing
callers. TCP publishes `FAILED` and suppresses automatic transport failover for
unknown data-RPC outcomes; the public batch API retains its existing status
reporting convention. Metadata, notification, and probe wrappers and the outer TCP retry loop no
longer replay calls or multiply the RPC retry/deadline budget. Legacy TCP retry/backoff
configuration remains accepted but cannot authorize extra attempts.

The source epoch is never refreshed during reconnect. A connection to a new
incarnation receives the original grant and fails with epoch mismatch. Dispatch
ownership, worker terminal publication, registration retention, and all 23
OrbitKV C API signatures remain unchanged. A checked build-directory overlay
adds only an idle-socket accessor to the pinned yalantinglibs header; it does not
modify the dependency checkout, framing, or write/completion owners. The #5132
dependency pin and consumer build flags are unchanged.

Regression coverage uses real loopback sockets: restart on the same port before
send; restart after the liveness check; changed-epoch READ/WRITE rejection; a
fully received request followed by EOF, reset, short header, or short body;
in-flight timeout; expired pre-send deadline; bounded address eviction and
active admission; and production TCP READ/WRITE no-replay with a configured
retry count of three. Notification/probe wrapper tests also exercise the
metadata-cache boundary and the engine RPC fallback. Server counters prove a lost reply does not cause a second
delivery. Existing assertions remain; the old stale-call failure test now
places the restart after the liveness check, and the metadata handler-count
test makes two explicit calls instead of depending on an automatic replay.

The baseline reproduction used the existing local image `orbitinfra-mooncake-validation:v0314`
(image ID `sha256:414f07ba1d7a25d8191c37c46eca52e74a2fca7ddf8d24fc79cb1d1eed9eab76`).
The fork build was `.git/validation/build-linux` at `a6caba7e`; reproducing that
baseline now requires a detached checkout at that commit. For the upstream
baseline, prepare a separate checkout and build inside this repository; retain the OrbitKV flags above with
`BUILD_UNIT_TESTS=ON` and build target `tent_tcp_regression_test`. Upstream's
standalone entry point needs an external CMake include declaring its CLI11
dependency, without modifying upstream source or tests:

```bash
mkdir -p .git/validation/tcp-stress
git worktree add --detach .git/validation/tcp-stress/upstream \
  e1ea5d31bb5e46ff2df9f90e7c2fbaf52fcdee8c
git -C .git/validation/tcp-stress/upstream submodule update --init --recursive
cat > .git/validation/tcp-stress/upstream-cli11.cmake <<'CMAKE'
if(PROJECT_NAME STREQUAL "mooncake-transfer-engine")
  include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Dependencies.cmake")
  mooncake_provide_cli11()
endif()
CMAKE
```

Inside the same image, with the repository mounted at `/workspace`, the upstream
configuration/build commands are:

```bash
root=/workspace/.git/validation/tcp-stress
cmake -S "$root/upstream/mooncake-transfer-engine" -B "$root/build-upstream" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  '-DCMAKE_BUILD_RPATH=$ORIGIN' '-DCMAKE_INSTALL_RPATH=$ORIGIN' \
  -DBUILD_UNIT_TESTS=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF \
  -DWITH_RUST_EXAMPLE=OFF -DUSE_HTTP=OFF -DUSE_ETCD=OFF -DUSE_REDIS=OFF \
  -DUSE_TENT=ON -DWITH_METRICS=OFF -DUSE_CUDA=OFF \
  -DCMAKE_PROJECT_INCLUDE="$root/upstream-cli11.cmake" \
  -DFETCHCONTENT_SOURCE_DIR_YALANTINGLIBS=/workspace/.git/validation/build-linux/_deps/yalantinglibs-src
cmake --build "$root/build-upstream" --target tent_tcp_regression_test --parallel 2
```

These were the baseline commands, with the fork binary built at `a6caba7e`
(not the fixed current tree). Each complete suite ran in one process, keeping
failures visible while allowing all 200 repetitions to finish:

```bash
for revision in fork upstream; do
  if [ "$revision" = fork ]; then
    binary=/workspace/.git/validation/build-linux/tent/tests/tent_tcp_regression_test
  else
    binary=/workspace/.git/validation/tcp-stress/build-upstream/tent/tests/tent_tcp_regression_test
  fi
  if docker run --rm --security-opt seccomp=unconfined \
    --mount "type=bind,source=$PWD,target=/workspace" -w /workspace \
    orbitinfra-mooncake-validation:v0314 "$binary" --gtest_repeat=200 \
    > ".git/validation/tcp-stress/$revision-200.log" 2>&1; then
    echo "$revision: exit 0"
  else
    echo "$revision: exit $?"
  fi
done
```

Both observed runs exited 1. Failure counts and first failing repetitions depend
on randomly selected ports. Raw logs and diagnostic sources remain outside Git
under `.git/validation/tcp-stress/`: `fork-200.log`, `upstream-200.log`,
`fork-pool-diagnostic.log`, `upstream-pool-diagnostic.log`, and `comparison.json`.
The SHA-256 digests of the two full-suite logs are respectively
`0de183c120995f963f78fdb91dd0f8412e4d6fe1e64669f9aa13fee631f4b899` and
`25ffeb98415198441d189a570e11b9c9d02767681891d8430f54ba2c1ce70c40`.


The fixed tree passed 200 complete repetitions of the expanded TCP regression
suite: **2,600/2,600 case invocations passed, zero failures** (13 cases per
repetition). `PayloadCapsRejectBeforeAllocationOrDataRpc` and
`ServerChecksEveryRegistrationPermission` each passed 200/200. The other TCP
cases include the new epoch/reconnect, lost-reply no-replay, and expired-deadline
controls. RPC reconnect tests also passed 50 repetitions (550/550 cases), and
metadata tests passed 50 repetitions (400/400 cases). The final formatted tree
passed the selected CPU production/ABI gate (19/19 CTest invocations; 460 cases
passed, three RDMA availability/hardware cases skipped), then the
200-repetition TCP loop. The combined reproduction command is:

```bash
mkdir -p .git/validation/tcp-reconnect-fix
docker run --rm --security-opt seccomp=unconfined \
  --mount "type=bind,source=$PWD,target=/workspace" -w /workspace \
  orbitinfra-mooncake-validation:v0314 bash -c '
    apt-get update && apt-get install -y patchelf &&
    USE_CUDA=OFF BUILD_JOBS=2 scripts/ci/run_orbitkv_tent_checks.sh .git/validation/build-linux &&
    .git/validation/build-linux/tent/tests/tent_tcp_regression_test --gtest_repeat=200 &&
    .git/validation/build-linux/tent/tests/tent_rpc_reconnect_test --gtest_repeat=50 &&
    .git/validation/build-linux/tent/tests/tent_segment_desc_retry_test --gtest_repeat=50
  ' > .git/validation/tcp-reconnect-fix/validation.log 2>&1
```

`patchelf` is installed only in the disposable container; the hosted workflow
already installs it. Raw logs remain outside Git in
`.git/validation/tcp-reconnect-fix/`: `cpu-gate-complete.log`,
`tcp-200-complete.log`, `rpc-50.log`, `metadata-50.log`, and `results.json`.
The final TCP stress log has SHA-256
`cad497a945699ca75ed3d4c0899c492487ace477e568264de5186c6f8a389bd6`.
Physical GPU, RDMA, and GDS behavior is not
established by these CPU/socket tests or the hosted CUDA fault-injection leg.

## Consuming this branch

In a separate OrbitKV session, update the submodule commit and branch metadata,
then update `SOURCE_VERSION` and `SOURCE_REVISION` in `orbitkv-mooncake-sys` to
match the reviewed new pin. Preserve all existing FFI signatures and layouts;
no Rust FFI or CMake flag change is needed. Add
`("tent/src/metrics/libtent_metrics.so", "libtent_metrics.so")` to `build.rs`'s
`RUNTIME_LIBRARIES` and to any installed/prebuilt artifact manifests. Stage and
rebuild all four shared libraries from the new source together. Do not reuse a
yalantinglibs override or build cache pinned to `7801bc9`; the release fetches
the new pin automatically.

Peers must use the matching OrbitKV TENT wire contract. Do not mix this branch
with stock upstream TCP peers, which lack the epoch envelope. Re-run OrbitKV's
native/installed-runtime ownership and hardware gates before accepting its
submodule bump. Do not force-push, rename, or delete the pinned integration
branch. This update does not change OrbitKV or the fork's main branch.
