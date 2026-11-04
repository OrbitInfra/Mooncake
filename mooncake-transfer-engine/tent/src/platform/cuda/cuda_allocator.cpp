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
#include <cuda.h>
#include <cuda_runtime.h>
#include <glog/logging.h>
#include <mutex>
#include <numa.h>
#include <vector>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace mooncake {
namespace tent {
namespace {

// Driver-API probe: true iff this process already has a primary context on
// `device`. Does not create one. Topology lists every visible GPU for NIC
// affinity; cudaSetDevice on those names would allocate idle-card contexts
// when the process can see more GPUs than this rank uses.
bool cudaPrimaryContextIsActive(int device) {
    if (!ensureCudaDriverInit()) return false;
    CUdevice cu_dev = 0;
    if (cuDeviceGet(&cu_dev, device) != CUDA_SUCCESS) return false;
    unsigned int flags = 0;
    int active = 0;
    if (cuDevicePrimaryCtxGetState(cu_dev, &flags, &active) != CUDA_SUCCESS) {
        return false;
    }
    return active != 0;
}

// Bare cudaGetDevice() can implicitly create GPU 0 when this thread has no
// current context. Only save/restore the caller's device when one exists.
bool cudaHasCurrentContext() {
    if (!ensureCudaDriverInit()) return false;
    CUcontext ctx = nullptr;
    return cuCtxGetCurrent(&ctx) == CUDA_SUCCESS && ctx != nullptr;
}

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

Status CudaPlatform::allocatePinnedHost(void** pptr, size_t size) {
    // Portable, so copies against any device's memory run as DMA. Like any
    // CUDA runtime call this initializes the calling thread's current device
    // if it has no context yet.
    CHECK_CUDA(cudaHostAlloc(pptr, size, cudaHostAllocPortable));
    return Status::OK();
}

Status CudaPlatform::freePinnedHost(void* ptr, size_t size) {
    (void)size;
    CHECK_CUDA(cudaFreeHost(ptr));
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
    const int device_id =
        destination_device >= 0 ? destination_device : source_device;
    if (device_id < 0) {
        ::memcpy(dst, src, length);
        return Status::OK();
    }

    constexpr size_t max_copy_calls = 64;
    constexpr size_t max_copy_bytes = size_t{1} << 30;
    if (length > max_copy_bytes)
        return Status::TooManyRequests("CUDA copy byte admission");
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
        if (copy_bytes_.compare_exchange_weak(bytes, bytes + length,
                                              std::memory_order_relaxed))
            break;
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

    // cudaMemcpyAsync() validates the stream against the current device's
    // context, so the copy has to run with the buffer's device pinned. A
    // device ordinal alone cannot restore the caller afterwards: it cannot
    // tell a custom non-primary CUcontext apart from the primary one, and a
    // thread that had no current context must not be left holding the one
    // this copy creates. Save the exact identity through the driver API.
    CUcontext caller_ctx = nullptr;
    if (cuCtxGetCurrent(&caller_ctx) != CUDA_SUCCESS) {
        return Status::InternalError(std::string("cuCtxGetCurrent failed") +
                                     LOC_MARK);
    }
    // Bare cudaGetDevice() can implicitly create GPU 0 when this thread has
    // no current context; only query the runtime binding when one exists.
    int caller_device = -1;
    if (caller_ctx != nullptr) {
        CHECK_CUDA(cudaGetDevice(&caller_device));
    }

    Status status;
    cudaError_t err = cudaSetDevice(device_id);
    if (err != cudaSuccess) {
        status = Status::InternalError(std::string("cudaSetDevice: ") +
                                       cudaGetErrorString(err) + LOC_MARK);
    } else {
        // The handle returns the stream to the pool at the end of this scope,
        // before the caller's state is restored below.
        CUDAStreamHandle stream;
        status = getStreamFromPool(stream, device_id);
        if (status.ok()) {
            err = cudaMemcpyAsync(dst, src, length, cudaMemcpyDefault,
                                  stream.get());
            if (err == cudaSuccess) err = cudaStreamSynchronize(stream.get());
            if (err != cudaSuccess) {
                // CUDA errors may report preceding async work. Keep the stream,
                // admitted bytes and the caller's RPC scratch alive.
                constexpr char message[] =
                    "TENT: CUDA copy termination unproven; retaining stream, "
                    "buffers and admission\n";
                (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
                while (true) (void)::poll(nullptr, 0, -1);
            }
        }
    }

    // Restore on every exit path, success or failure. cudaSetDevice() first
    // brings the runtime binding back (and may swap in that device's primary
    // context), then cuCtxSetCurrent() restores the exact context identity: a
    // custom CUcontext, or none at all for a caller that started bare.
    bool restore_failed = false;
    if (caller_device != -1 && caller_device != device_id) {
        restore_failed = cudaSetDevice(caller_device) != cudaSuccess;
    }
    const CUresult restore_rc = cuCtxSetCurrent(caller_ctx);
    if (restore_failed || restore_rc != CUDA_SUCCESS) {
        constexpr char message[] =
            "TENT: CUDA context restoration failed; retaining worker and copy "
            "admission\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        while (true) (void)::poll(nullptr, 0, -1);
    }
    if (!status.ok()) {
        LOG(ERROR) << "CudaPlatform::copy failed: " << status.message();
    }
    return status;
}

Status CudaPlatform::synchronizeDevices(const Topology* topology) {
    const std::vector<int> devices =
        topologyDeviceIndices(topology, Topology::MEM_CUDA);
    if (devices.empty()) return Status::OK();

    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count <= 0) {
        if (err != cudaSuccess) {
            LOG(WARNING) << "CudaPlatform::synchronizeDevices "
                            "cudaGetDeviceCount failed: "
                         << cudaGetErrorString(err);
            (void)cudaGetLastError();
        }
        return Status::OK();
    }

    int saved = 0;
    const bool have_saved =
        cudaHasCurrentContext() && cudaGetDevice(&saved) == cudaSuccess;
    if (!have_saved) (void)cudaGetLastError();

    for (int device : devices) {
        if (device >= device_count) continue;
        if (!cudaPrimaryContextIsActive(device)) continue;
        err = cudaSetDevice(device);
        if (err != cudaSuccess) {
            LOG(WARNING) << "CudaPlatform::synchronizeDevices cudaSetDevice("
                         << device << ") failed: " << cudaGetErrorString(err);
            (void)cudaGetLastError();
            continue;
        }
        err = cudaDeviceSynchronize();
        if (err != cudaSuccess) {
            LOG(WARNING)
                << "CudaPlatform::synchronizeDevices cudaDeviceSynchronize "
                   "device "
                << device << " failed: " << cudaGetErrorString(err);
            (void)cudaGetLastError();
        }
    }
    if (have_saved) {
        err = cudaSetDevice(saved);
        if (err != cudaSuccess) {
            LOG(WARNING)
                << "CudaPlatform::synchronizeDevices restore cudaSetDevice("
                << saved << ") failed: " << cudaGetErrorString(err);
            (void)cudaGetLastError();
        }
    }
    return Status::OK();
}
}  // namespace tent
}  // namespace mooncake
