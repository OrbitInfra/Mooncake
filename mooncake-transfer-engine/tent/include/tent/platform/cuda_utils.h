// Copyright 2026 KVCache.AI
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

#ifndef TENT_PLATFORM_CUDA_UTILS_H
#define TENT_PLATFORM_CUDA_UTILS_H

#include <cuda.h>

#include <mutex>

namespace mooncake {
namespace tent {

// cuInit() is idempotent and creates no context; the driver API does not
// initialize itself lazily, so it must run before any driver query.
inline bool ensureCudaDriverInit() {
    static std::once_flag flag;
    static CUresult result = CUDA_ERROR_NOT_INITIALIZED;
    std::call_once(flag, []() { result = cuInit(0); });
    return result == CUDA_SUCCESS;
}

// On CUDA_SUCCESS, `device` is the device ordinal, or -1 for host memory.
// Errors never classify a pointer as host. Unlike the runtime
// cudaPointerGetAttributes(), this never implicitly initializes the calling
// thread's current device and its primary context (default GPU 0).
inline CUresult getCudaDeviceForPtr(const void* ptr, int& device) {
    device = -1;
    if (!ensureCudaDriverInit()) return CUDA_ERROR_NOT_INITIALIZED;
    unsigned int mem_type = 0;
    int ordinal = -1;
    CUpointer_attribute attributes[] = {CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
                                        CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL};
    void* values[] = {&mem_type, &ordinal};
    const auto result = cuPointerGetAttributes(
        2, attributes, values, reinterpret_cast<CUdeviceptr>(ptr));
    if (result != CUDA_SUCCESS) return result;
    if (mem_type == 0 || mem_type == CU_MEMORYTYPE_HOST) return CUDA_SUCCESS;
    if ((mem_type != CU_MEMORYTYPE_DEVICE &&
         mem_type != CU_MEMORYTYPE_UNIFIED) ||
        ordinal < 0)
        return CUDA_ERROR_INVALID_VALUE;
    device = ordinal;
    return CUDA_SUCCESS;
}

}  // namespace tent
}  // namespace mooncake

#endif  // TENT_PLATFORM_CUDA_UTILS_H
