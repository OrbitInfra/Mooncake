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
unknown data-RPC outcomes. The existing logical batch status enum is unchanged;
`tent_terminal_result` now exposes the native code and effect certainty (below). Metadata, notification, and probe wrappers and the outer TCP retry loop no
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

## Native status codes at the C boundary (OrbitKV patch)

OrbitKV #314 found that the C wrapper reduced every native failure to `-1`,
and that logical `FAILED` completion discarded the data RPC's failure code.
Generic integer-returning C operations now return `0` for success or the
**negative native `Status::Code`**, including `-134` (before dispatch), `-135`
(unknown effect after dispatch), and `-2` (capacity/admission rejection).
This applies to open/close/info, memory allocation and registration, submit and
submit-with-notification, notifications, polling, cancellation, NIC stats, and
remote epoch validation. Invalid arguments remain `-1`. Caught exceptions and
wrapper allocation failures use `-199`. The boolean, pointer/handle, source
close/drain, and positive `TENT_FILE_*` contracts are unchanged.

All 23 consumed signatures and all existing C struct layouts remain unchanged.
One C function and four constants are added:

```c
#define TENT_ERROR_TOO_MANY_REQUESTS 2
#define TENT_ERROR_RPC_CONNECTION 134
#define TENT_ERROR_RPC_UNKNOWN_EFFECT 135
#define TENT_ALL_TASKS SIZE_MAX
int tent_terminal_result(tent_engine_t engine, tent_batch_id_t batch_id,
                          size_t task_id, int* status_code, int* effect_known);
```

The new query observes cached terminal evidence under the batch progress lock;
it does not poll, dispatch, retry, or release resources. Use the existing status
polls to progress the engine first. The return value is `0` for a terminal
receipt, `-2` while pending, or `-1` for an invalid/freed batch or task. Outputs
start at `135/0` on query failure. Successful outputs are a **positive** native
code (zero for completed success), and `effect_known` as zero or one.

TCP preserves every worker failure code before terminal publication. Its
pre-dispatch connection/admission failures have a known unsent effect; an
ambiguous data RPC retains `135/0` and suppresses failover. Other native failures
keep their code without inventing certainty. Cancellation, timeout, and older
backends without terminal evidence conservatively produce `135/0`. A later
rejected attempt does not erase uncertainty from an earlier failover attempt.
Merged aliases report the physical owner's evidence. For `TENT_ALL_TASKS`, all
tasks must be terminal: 135 takes precedence over other errors, otherwise the
first nonzero code in task order is returned; certainty is the AND of every
task's result. A successful sibling may already have modified its destination.

These are logical outcome receipts, **not physical drain receipts**. Neither
an error code nor `effect_known` authorizes releasing an owned buffer or
replaying a whole batch. The source and file physical-drain APIs still apply.
OrbitKV can add this symbol to its FFI and classify native codes instead of
assuming every `-1` is uncertain; no existing FFI signature or build flag changes
are needed. C++ `TransferStatus` carries extra terminal evidence internally;
C++ users must rebuild with this branch, while C `tent_status_t` is unchanged.

Local validation used the same Debian trixie/aarch64 container, Linux
6.12.76-linuxkit, GCC 14.4.0, with `--security-opt seccomp=unconfined` and the
OrbitKV build flags listed above. The shared-library C gate passed 8/8 cases,
then 50 repetitions (400 invocations, zero failures). The ABI gate passed,
including all 23 original declarations/layouts and the new exported symbol.
The unchanged full TCP suite passed 200 repetitions: 2,600 invocations, zero
failures, including the payload-cap and registration-permission regressions.

```sh
cmake -S mooncake-transfer-engine -B .git/validation/build-linux \
  -DBUILD_UNIT_TESTS=ON
cmake --build .git/validation/build-linux --parallel 1 --target \
  tent_orbitkv_c_status_test tent_orbitkv_abi_test tent_tcp_regression_test
.git/validation/build-linux/tent/tests/tent_orbitkv_c_status_test --gtest_repeat=50
.git/validation/build-linux/tent/tests/tent_orbitkv_abi_test
.git/validation/build-linux/tent/tests/tent_tcp_regression_test --gtest_repeat=200
```

The C gate covers refusal before dispatch (134), EOF during descriptor lookup
(135), both submit wrappers rejecting capacity (2), epoch validation, and
EOF/reset/short-header/short-body after READ and WRITE dispatch. It checks
request/connection counts to reject replay, stable task and aggregate receipts,
merged aliases, success, pending, invalid/freed IDs, and conservative
cancel/timeout evidence. Segment open remains lazy; lookup/epoch validation is
what actually sends the descriptor RPC. Logs stay outside Git under
`.git/validation/c-status/`. Scoped pre-commit passes.

The hosted `orbitkv-integration.yml` gate also includes the new C test in its
CPU leg. CUDA/cuFile checks remain compilation and injected controls; real
GPU, RDMA and GDS profiles are not qualified here.

Upstream main `4704c67f` has the same synchronous `Status` collapse. A separate
`fix/tent-c-status-codes` branch carries only that shared correction, its header
contract and generic C tests; it excludes this branch's epoch, file and
terminal-effect additions.

## File I/O C API (OrbitKV patch)

OrbitKV #312 identified that the old initializer disabled both file transports
and that generic batch status/free was not a physical-drain receipt. The new
API drives TENT's IOURING/GDS sub-batches directly, with no transport failover or
WRITE replay. It does not use OrbitKV's POSIX SSD byte-movement path. All 23
existing OrbitKV C signatures and their struct layouts remain unchanged.

The declarations below are additive in
[`tent/transfer_engine.h`](mooncake-transfer-engine/tent/include/tent/transfer_engine.h).
The header defines `tent_file_options_t` (struct size, io_uring enable, GDS mode,
and the two queue depths), `tent_file_range_t` (address and length),
`tent_file_owner_t` (context, retain and release callbacks), and
`tent_file_result_t` (outcome, drained flag and transferred bytes).
`tent_file_submitv` adds typed elements for OrbitKV A2 (#316), so its file
adapter can hand TENT padding and fragmented host/GPU ranges without a scratch
buffer. Existing functions and layouts, including `tent_file_range_t`, are
unchanged; this file API is fork-only.

```c
int tent_create_engine_with_files(const tent_file_options_t* options,
                                  tent_engine_t* engine);
int tent_file_open(tent_engine_t engine, const char* uri,
                   tent_file_access_t access, int transport,
                   tent_file_id_t* file, uint64_t* length);
int tent_file_submit(tent_engine_t engine, tent_file_id_t file, int opcode,
                     uint64_t file_offset, const tent_file_range_t* ranges,
                     size_t count, const tent_file_owner_t* owner,
                     tent_file_batch_t* batch);
typedef enum tent_file_element_kind {
    TENT_FILE_ELEMENT_RANGE = 0,
    TENT_FILE_ELEMENT_PADDING = 1
} tent_file_element_kind_t;
typedef struct tent_file_element {
    tent_file_element_kind_t kind;
    void* address;
    uint64_t length;
} tent_file_element_t;
int tent_file_submitv(tent_engine_t engine, tent_file_id_t file, int opcode,
                      uint64_t file_offset,
                      const tent_file_element_t* elements, size_t count,
                      const tent_file_owner_t* owner, tent_file_batch_t* batch);
int tent_file_drain(tent_engine_t engine, tent_file_batch_t batch,
                    uint64_t timeout_ms, int cancel,
                    tent_file_result_t* result);
int tent_file_release_batch(tent_engine_t engine, tent_file_batch_t batch);
int tent_file_close(tent_engine_t engine, tent_file_id_t file,
                    uint64_t timeout_ms, tent_file_result_t* result);
```

### Configuration and addressing

Zero-initialize the options, set `struct_size = sizeof(tent_file_options_t)`,
`io_uring_enabled` to 0 or 1, and `gds_mode` to `TENT_GDS_OFF`, `TENT_GDS_ON`, or
`TENT_GDS_NATIVE_ONLY`. Each enabled transport requires a queue depth in
1..1024. Creation snapshots the existing thread-local TENT settings and then
overrides the file enable/depth settings; it does not mutate those settings.
An enabled transport that was not built, cannot install, or lacks the required
runtime returns `TENT_FILE_UNAVAILABLE` with a null engine. Disabled transports
return `TENT_FILE_DISABLED` at file open; they never silently fall back.

`tent_file_open` accepts an existing regular `file://` path, including
`file:///proc/self/fd/N`, READ, WRITE, or READ_WRITE access, and an explicit
`TRANSPORT_IOURING` or `TRANSPORT_GDS`. It pins that inode, returns its current
length, and opens the native descriptor with the corresponding OS permissions.
Unlinking/replacing the original path does not redirect an admitted operation.
The file ID is engine-local and is not interchangeable with a generic segment
ID. Files must already have their allocated length; submission rejects an
out-of-bounds range or integer overflow before retaining a token or dispatching.
A subsequent external truncation can still produce a short-read receipt.

A vector maps its elements consecutively from `file_offset`. A RANGE element
has a nonnull address and a nonzero length. A PADDING element has a **null**
address and a nonzero length: WRITE emits zeros; READ consumes and discards
file bytes without touching caller memory. Padding counts toward bounds,
admission limits and the completed file prefix. An all-padding vector still
requires an owner token, retaining the caller's extent reservation.

`tent_file_submitv` gathers/scatters through TENT-owned scratch in windows of at
most 4 MiB, submitting one contiguous request at a time. Its receipt is the
**completed file prefix including padding**. A short READ scatters only that
prefix, even when EOF falls inside padding; later ranges stay unchanged.
Unaligned vectors passed to the original `tent_file_submit` also use staging.
Aligned legacy vectors retain their parallel windows: requests are split at
16 MiB and dispatched up to the selected queue depth, with sum-of-ranges
receipts that need not be a contiguous prefix. Neither API is atomic; a
partial WRITE is never replayed or sent to another backend.

IOURING accepts odd host/GPU addresses, lengths and file offsets. The explicit
file owner opens both direct and buffered descriptors on the pinned inode.
Before dispatch it selects buffered io_uring for an extent whose offset or
length is not 4 KiB aligned, and direct I/O otherwise (buffered when the
filesystem rejects opening O_DIRECT). It never rounds a WRITE outward or
performs read/modify/write on neighboring bytes. The generic upstream file
context is unchanged; this is a fork file-API capability extension.

Admission is bounded per engine: 256 open/opening/retained files, 32 live batch
receipts, 16384 input ranges and 64 MiB per batch. Active and uncertain batches
continue consuming their slot and byte allowance; even a completed receipt
occupies its slot until `tent_file_release_batch`. There is one worker per
admitted batch and at most one cleanup worker per closing file. File handles
use explicit native owners, not the unbounded legacy file-descriptor caches.
Vector staging additionally reserves a shared **64 MiB per-engine budget**
before admission; exhausted budget returns `TENT_FILE_BUSY` with no callback
or I/O. IOURING charges its host window; GDS charges both its host zero-fill
window and GPU window (at most 8 MiB combined). Charges survive timeout,
uncertain copies and failed cleanup. They return only after physical I/O and
copies finish and scratch cleanup succeeds. A completed receipt keeps its
batch slot but no scratch charge.

### Ownership, cancellation and physical drain

The owner callbacks must retain the actual host/GPU backing, registrations,
extent reservation, CUDA context when applicable, and admission charge. The
caller grants immutable WRITE sources or exclusive READ destinations through
physical drain, and establishes GPU producer readiness before submission.
Callbacks must be nonthrowing and nonblocking, must not reenter the file API,
and support
execution on a worker thread; release cannot assume a caller CUDA context.
Cleanup of caller registrations remains the caller's responsibility and a
failed unregister must retain its owner. TENT separately owns and deregisters
its GPU staging; a failed deregister retains that window and its charge. The
callback releases TENT's reference, not a
promise that the caller's other references or registrations have been freed.

A successful submit calls retain exactly once before dispatch. Rejection
returns a zero batch ID and runs neither callback. The worker keeps the token,
file, transport, staging buffers and native sub-batch through all admitted
I/O. For IOURING it observes every CQE before entering #5068's sub-batch free;
for GDS it waits for every slice to become terminal, including siblings of a
failed slice. A handoff to GDS quarantine is never a physical-drain receipt.
The io_uring path now checks both staging copy results; an uncertain CUDA copy
retains its worker and owners under the existing fork copy contract.

`cancel=1`, close, and a positive drain timeout stop admission of subsequent
windows. The window already being dispatched may finish: this is best-effort
logical cancellation, not a kernel cancellation receipt. `timeout_ms=0` only
polls and does not impose a new deadline; positive values are bounded to one
day. Drain and close do not wait indefinitely for a blocked provider, staging
copy or file cleanup. They return `TENT_FILE_UNQUIESCED` with `drained=0` when physical
termination has not been proved. The batch, engine and file stay owned and
charged. Polling completion again is allowed; **resubmitting the WRITE is not**.
A failed native cleanup is retained permanently and is never attempted twice.

After every I/O and staging operation has finished, sub-batch cleanup succeeds,
and release has returned, drain reports `drained=1`. Its return code equals the
logical outcome: `OK`, `SHORT_IO`, `IO_ERROR`, `CANCELED`, or `TIMEOUT` (all have
the `TENT_FILE_` prefix). A deadline can therefore be followed by a drained
TIMEOUT receipt. Before drain, an OK outcome means only that no error has yet
been observed; it is not completion. Pre-admission errors additionally include
`INVALID_ARGUMENT`, `PERMISSION`, `DISABLED`, `UNAVAILABLE`, and `BUSY`.

Close first closes file admission, requests logical cancellation, and drains
all its batches within one shared deadline. It then closes the native file
owner on a bounded cleanup worker. Only a successful close reports the file
released; failed/uncertain close retains the handle and supports later
observation without retrying a possibly partial cleanup. Individual batch
receipts remain available until explicitly released. `tent_destroy_engine`
retains an engine with any outstanding file handles or batch receipts instead
of deleting it. Close/release all of them before destroying the engine. As for
the original C API, callers must keep the engine alive through concurrent API
calls and serialize engine destruction.

The existing `tent_close_source` proof remains restricted to CPU TCP-only
engines. Enabling file transports does **not** upgrade that proof: it still
returns unsupported. OrbitKV should create a separate engine for file I/O,
leaving its current network engine and epoch/grant fencing intact.

### CUDA and GDS limits

IOURING supports host buffers and TENT's CUDA-through-host-staging path. GDS ON
uses cuFile's configured routing, which may include compatibility I/O. GDS
NATIVE_ONLY fails closed unless live cuFile queries prove both
`allow_compat_mode` and `force_compat_mode` disabled, and batch I/O is available.
It currently accepts only local ext4/XFS files and NVIDIA device RANGE
elements; managed and host memory are rejected. Fragmented/unaligned device
ranges and padding are gathered/scattered through a registered, aligned GPU
window. Native-only still requires a 4 KiB-aligned file offset and total
extent length. GDS ON permits cuFile's configured compatibility routing for
odd extents; neither mode silently falls back to IOURING. Native-only windows
are also capped by the live
cuFile bounce-cache and direct-I/O limits, because an oversized batch slice can
otherwise take compatibility I/O. Interior ranges of memory registered with
this engine preserve cuFile's registered base plus buffer offset. Make GDS
registrations through the file engine; registrations made externally to TENT
are outside this API's supported registration contract. Older SDKs without the policy-query API return
unavailable for this mode. No process-global CUDA/cuFile configuration is
changed. This checks policy; native-route qualification still requires hardware
and route evidence, as described in the
[NVIDIA cuFile reference](https://docs.nvidia.com/gpudirect-storage/api-reference-guide/).
GDS open/submit require a current CUDA context; retain the opening context until
file close and the submitting context in each batch token until drain.

This API covers OrbitKV's preallocated shard files, retained exact inode,
scatter/gather byte movement, read/write permissions, deadline/cancel and
physical-drain receipts. It does not create/extend shards, allocate extents,
encode records, publish indexes, provide fsync durability, make a vector atomic,
or run record codecs. Those remain consumer work. OrbitKV can replace its
TENT-only scratch adapter with `tent_file_submitv`: map data to RANGE, gaps and
tails to PADDING, and retain the backing/extent token through drain. No caller
scratch, zero fill, gather/scatter or padding copy is required. Add the new
element type and symbol to its FFI separately; every non-drained result maps
to `Unquiesced`, and WRITE publication requires a full-byte drained success.

### File API validation

The real-file C boundary gate is `tent_orbitkv_file_io_test`; it links the
shared library and includes a translation unit compiled as C. It requires
working io_uring and fails instead of silently skipping a blocked kernel.
It covers byte-checked vector round trips, permission/overflow rejection,
short reads and EOF with unchanged suffixes, cancel and deadline drain,
retained actual backing after a lost completion, capacity retention, inode
replacement, delayed file close, and non-retried failed cleanup.
The padding/vector additions cover nonzero-file gap/tail zeroing, READ
discard, odd legacy ranges and file offsets, a short READ inside padding,
cross-window vectors, cancel during a GPU-copy shim, lost completion and
failed cleanup retaining scratch, and shared-budget exhaustion/recovery.
The GPU shim drives the production FileIO owner and real io_uring bytes; it
does not claim a physical GPU copy or native GDS route.

Padding/vector validation on the Linux container described below, with
`--security-opt seccomp=unconfined`: 22/22 CPU CTest entries passed (523 case
invocations: 520 passed, three RDMA availability/hardware skips). The expanded
file suite passed **26 cases × 50 repetitions on each filesystem**, 2600 total
invocations with no failures or skips. The full TCP stress passed **13 cases ×
200 repetitions**, 2600 invocations with no failures. After the CPU build,
these were the repeated commands inside the container:

```bash
.git/validation/build-linux/tent/tests/tent_orbitkv_file_io_test --gtest_repeat=50
TENT_FILE_TEST_DIR=/tmp .git/validation/build-linux/tent/tests/tent_orbitkv_file_io_test --gtest_repeat=50
.git/validation/build-linux/tent/tests/tent_tcp_regression_test --gtest_repeat=200
```

The padding extension's logs and counts are under
`.git/validation/file-padding/` (outside Git). The hosted workflow runs the
same 26 file cases on both filesystems in its CPU leg, and compiles the new
API with CUDA/cuFile in its CUDA leg. Actual GPU copies and GDS routes remain
unverified without hardware.

Initial file-API results (before the padding extension) on Debian trixie aarch64, Linux
6.12.76-linuxkit, GCC 14.4.0, and CMake 3.31.6:

| Gate | Result |
| --- | --- |
| CPU production/relocated-library/ABI and selected TENT tests | 21/21 CTest invocations passed; 488 GoogleTest case invocations passed, three RDMA availability/hardware cases skipped. |
| File C API, real io_uring | 14/14 cases on tmpfs and 14/14 on the regular container filesystem (`/tmp`); no skips. |
| CUDA 13.0.2 + cuFile 1.15.1.6 build and injected controls | 4/4 CTest invocations, 33/33 cases passed. Includes 18 GDS policy/completion controls and managed-memory rejection; no GPU I/O is claimed. |
| Full TCP regression stress | 200 repetitions, 2600/2600 case invocations passed, zero failures. |
| Scoped pre-commit, clang-format 20.1.8, CMake format, diff check | Passed. |

The CPU commands were:

```bash
docker run --rm --security-opt seccomp=unconfined \
  --mount "type=bind,source=$PWD,target=/workspace" -w /workspace \
  orbitinfra-mooncake-validation:v0314 bash -c '
    apt-get update -qq && apt-get install -y -qq patchelf &&
    USE_CUDA=OFF BUILD_JOBS=2 scripts/ci/run_orbitkv_tent_checks.sh .git/validation/build-linux &&
    .git/validation/build-linux/tent/tests/tent_tcp_regression_test --gtest_repeat=200
  '
```

The CUDA build used the existing CUDA 13.0.2 toolkit and driver stubs under
`.git/validation/cuda-redist`, plus the NVIDIA cuFile 1.15.1.6 aarch64 archive:

```bash
docker run --rm --security-opt seccomp=unconfined \
  --mount "type=bind,source=$PWD,target=/workspace" -w /workspace \
  orbitinfra-mooncake-validation:v0314 bash -c '
    apt-get update -qq && apt-get install -y -qq patchelf &&
    export CUFILE_ROOT=/workspace/.git/validation/file-io/libcufile-linux-sbsa-1.15.1.6-archive &&
    export LD_LIBRARY_PATH=$CUFILE_ROOT/lib:/workspace/.git/validation/cuda-redist/lib/stubs:/workspace/.git/validation/cuda-redist/lib &&
    USE_CUDA=ON BUILD_JOBS=2 scripts/ci/run_orbitkv_tent_checks.sh .git/validation/build-linux-cuda
  '
```

The maintained fork workflow runs the same CPU gate on x86-64 Linux and now
also compiles cuFile/GDS and runs its injected controls on the CUDA leg. Its
NVIDIA archive is pinned by SHA-256. GDS-enabled builds retain upstream's
link-time cuFile dependency: cuFile must be installed even if a particular
engine disables GDS. No new dependency is needed for an IOURING-only build.

Raw logs and XML stay in `.git/validation/file-io/`, outside Git:
`final-cpu.log`, `final-cuda.log`, `final-tcp-200.log`, `pre-commit.log`, and
`results.json`. The final TCP log SHA-256 is
`a80554f2f891909b3dd3981b721a7ccd0529a43fd6a2e0701dd64e12ec0409e9`.
GPU staging and native GDS are source/compile and injected-control coverage
only until installed hardware gates run; this patch makes no GPU, RDMA or GDS
hardware claim.

## Consuming this branch

In a separate OrbitKV session, update the submodule commit and branch metadata,
then update `SOURCE_VERSION` and `SOURCE_REVISION` in `orbitkv-mooncake-sys` to
match the reviewed new pin. Preserve all existing FFI signatures and layouts;
existing network FFI needs no signature change. The file API requires the
additive FFI and consumer integration described above. Add
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
