// Copyright 2025 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tent/platform/cuda.h"
#include "tent/platform/cuda_utils.h"
#include "tent/common/status.h"

#include <bits/stdint-uintn.h>
#include <cuda_runtime.h>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <numa.h>
#include <glog/logging.h>

namespace mooncake {
namespace tent {
namespace {
}  // namespace

Status CudaPlatform::allocate(void** pptr, size_t size,
                              MemoryOptions& options) {
    LocationParser location(options.location);
    if (location.type() == "cuda") {
        int cuda_dev = 0;
        CHECK_CUDA(cudaGetDevice(&cuda_dev));
        CHECK_CUDA(cudaSetDevice(location.index()));
        CHECK_CUDA(cudaMalloc(pptr, size));
        cudaSetDevice(cuda_dev);
        return Status::OK();
    }
    int socket_id = 0;
    if (location.type() == "cpu") socket_id = location.index();
    *pptr = numa_alloc_onnode(size, socket_id);
    if (!(*pptr))
        return Status::InternalError("Unable to allocate DRAM memory");
    return Status::OK();
}

Status CudaPlatform::free(void* ptr, size_t size) {
    int device = -1;
    CHECK_STATUS(cudaPointerDevice(ptr, device));
    if (device >= 0) {
        CHECK_CUDA(cudaFree(ptr));
    } else {
        numa_free(ptr, size);
    }
    return Status::OK();
}

Status CudaPlatform::copy(void* dst, void* src, size_t length) {
    // Use cudaMemcpyAsync with a non-blocking stream instead of cudaMemcpy(),
    // as the latter relies on the legacy default stream and can introduce
    // unintended synchronization or even deadlocks in downstream
    // components (e.g. mooncake-pg).
    //
    // cudaMemcpyAsync routes the copy through its stream's device context, so
    // the stream must live on the device owning the device-side buffer.
    // Control-plane RPC worker threads sit on cuda:0 while a registered buffer
    // may live on cuda:R; taking the stream from the buffer's device routes the
    // copy correctly without mutating the calling thread's current device.
    // Host-only copies keep the current device.
    int destination_device = -1, source_device = -1;
    CHECK_STATUS(cudaPointerDevice(dst, destination_device));
    CHECK_STATUS(cudaPointerDevice(src, source_device));
    const int device_id = destination_device >= 0 ? destination_device : source_device;
    if (device_id < 0) {
        ::memcpy(dst, src, length);
        return Status::OK();
    }

    constexpr size_t max_copy_calls = 64;
    constexpr size_t max_copy_bytes = size_t{1} << 30;
    if (length > max_copy_bytes) return Status::TooManyRequests("CUDA copy byte admission");
    const size_t calls = copy_calls_.fetch_add(1, std::memory_order_relaxed);
    if (calls >= max_copy_calls) {
        copy_calls_.fetch_sub(1, std::memory_order_relaxed);
        return Status::TooManyRequests("CUDA copy call admission");
    }
    size_t bytes = copy_bytes_.load(std::memory_order_relaxed);
    while (true) {
        if (bytes > max_copy_bytes - length) {
            copy_calls_.fetch_sub(1, std::memory_order_relaxed);
            return Status::TooManyRequests("CUDA copy byte admission");
        }
        if (copy_bytes_.compare_exchange_weak(bytes, bytes + length, std::memory_order_relaxed)) break;
    }
    struct CopyAdmission {
        std::atomic<size_t>& calls;
        std::atomic<size_t>& bytes;
        size_t length;
        ~CopyAdmission() {
            bytes.fetch_sub(length, std::memory_order_relaxed);
            calls.fetch_sub(1, std::memory_order_relaxed);
        }
    } admission{copy_calls_, copy_bytes_, length};

    CUcontext saved_context = nullptr;
    CHECK_CU(cuCtxGetCurrent(&saved_context));
    struct ContextRestore {
        CUcontext context;
        ~ContextRestore() {
            if (cuCtxSetCurrent(context) != CUDA_SUCCESS) {
                constexpr char message[] = "TENT: CUDA context restoration failed; retaining worker and copy admission\n";
                (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
                while (true) (void)::poll(nullptr, 0, -1);
            }
        }
    } restore{saved_context};
    CHECK_CUDA(cudaSetDevice(device_id));
    Status result;
    {
        CUDAStreamHandle stream;
        result = getStreamFromPool(stream, device_id);
        if (result.ok()) {
            auto error = cudaMemcpyAsync(dst, src, length, cudaMemcpyDefault, stream.get());
            if (error == cudaSuccess) error = cudaStreamSynchronize(stream.get());
            if (error != cudaSuccess) {
                // CUDA errors may report preceding async work. Keep this
                // stream, admitted bytes and the caller's RPC scratch alive.
                constexpr char message[] = "TENT: CUDA copy termination unproven; retaining stream, buffers and admission\n";
                (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
                while (true) (void)::poll(nullptr, 0, -1);
            }
        }
    }
    return result;
}

}  // namespace tent
}  // namespace mooncake
