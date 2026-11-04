#!/usr/bin/env bash
# Build the OrbitKV standalone entry point, then qualify the carried contracts.
set -euo pipefail
build_dir=${1:-build-orbitkv}
cuda=${USE_CUDA:-OFF}
jobs=${BUILD_JOBS:-2}
file_args=()
if [[ -n "${CUFILE_ROOT:-}" ]]; then
  file_args=(-DCUFILE_INCLUDE="$CUFILE_ROOT/include"
             -DCUFILE_LIB="$CUFILE_ROOT/lib/libcufile.so")
fi
cmake -S mooncake-transfer-engine -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  '-DCMAKE_BUILD_RPATH=$ORIGIN' '-DCMAKE_INSTALL_RPATH=$ORIGIN' \
  -DBUILD_UNIT_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARK=OFF \
  -DWITH_RUST_EXAMPLE=OFF -DUSE_HTTP=OFF -DUSE_ETCD=OFF -DUSE_REDIS=OFF \
  -DUSE_TENT=ON -DWITH_METRICS=OFF -DUSE_CUDA="$cuda" "${file_args[@]}"
cmake --build "$build_dir" --target tent_shared --parallel "$jobs"
# Match OrbitKV's relocated runtime directory. v0.3.14 adds tent_metrics as a
# shared dependency even when metrics are disabled; keep the singleton shared.
runtime_dir="$build_dir/orbitkv-runtime"
mkdir -p "$runtime_dir"
for library in tent/src/libtent_shared.so \
               tent/src/metrics/libtent_metrics.so \
               mooncake-common-src/libmooncake_common.so \
               mooncake-common/libasio.so; do
  destination="$runtime_dir/${library##*/}"
  cp "$build_dir/$library" "$destination"
  patchelf --set-rpath '$ORIGIN' "$destination"
done
python3 - "$runtime_dir/libtent_shared.so" <<'PY'
import ctypes
import sys

ctypes.CDLL(sys.argv[1])
print("OrbitKV relocated four-library runtime loads successfully", flush=True)
PY
cmake -S mooncake-transfer-engine -B "$build_dir" -DBUILD_UNIT_TESTS=ON
if [[ "$cuda" == ON ]]; then
  targets=(tent_orbitkv_abi_test tent_orbitkv_cuda_ownership_test
           tent_cuda_plugin_context_test tent_cuda_plugin_context_gpu_test)
  # This runner has a toolkit and driver stubs, but no physical GPU.
  pattern='^tent_(orbitkv_abi|orbitkv_cuda_ownership|cuda_plugin_context)_test$'
  if [[ -n "${CUFILE_ROOT:-}" ]]; then
    targets+=(tent_gds_transport_status_test tent_orbitkv_file_io_test)
    pattern='^tent_(orbitkv_abi|orbitkv_cuda_ownership|cuda_plugin_context|gds_transport_status)_test$'
  fi
else
  targets=(tent_orbitkv_abi_test tent_orbitkv_file_io_test
           tent_tcp_transport_test tent_tcp_regression_test
           tent_tcp_datapath_roundtrip_test segment_manager_test
           tent_local_memory_lifecycle_test tent_endpoint_lifecycle_test
           tent_rdma_transport_test tent_io_uring_transport_test
           tent_segment_desc_retry_test tent_rpc_reconnect_test
           tent_rpc_handler_isolation_test tent_bootstrap_response_test
           tent_transport_selector_test request_merge_test
           tent_runtime_queue_dispatch_test tent_engine_failover_e2e_test
           tent_progress_worker_test)
  pattern='^(segment_manager_test|request_merge_test|tent_(orbitkv_abi|orbitkv_file_io|tcp_transport|tcp_regression|tcp_datapath_roundtrip|local_memory_lifecycle|endpoint_lifecycle|rdma_transport|io_uring_transport|segment_desc_retry|rpc_reconnect|rpc_handler_isolation|bootstrap_response|transport_selector|runtime_queue_dispatch|engine_failover_e2e|progress_worker)_test.*)$'
fi
cmake --build "$build_dir" --target "${targets[@]}" --parallel "$jobs"
mkdir -p "$build_dir/test-results/gtest"
build_dir=$(cd "$build_dir" && pwd)
export GTEST_OUTPUT="xml:$build_dir/test-results/gtest/"
ctest --test-dir "$build_dir" --output-on-failure --no-tests=error \
  --timeout 120 -R "$pattern" --output-junit "$build_dir/test-results/ctest.xml"
