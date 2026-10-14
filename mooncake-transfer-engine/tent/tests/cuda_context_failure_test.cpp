// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0.
// This control interposes CUDA APIs; it does not execute device operations.
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <iostream>
#include <cuda.h>
#include <cuda_runtime.h>
#include "tent/platform/cuda.h"
#include "tent/platform/cuda_utils.h"
namespace {
thread_local CUcontext current = reinterpret_cast<CUcontext>(0x777);
std::string mode;
std::array<unsigned char, 64> source{}, target{};
int device = 7;
unsigned restores = 0;
}  // namespace
extern "C" CUresult CUDAAPI cuInit(unsigned int) { return CUDA_SUCCESS; }
extern "C" CUresult CUDAAPI cuCtxGetCurrent(CUcontext* out) {
    *out = current;
    return mode == "query" ? CUDA_ERROR_INVALID_CONTEXT : CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuCtxSetCurrent(CUcontext saved) {
    ++restores;
    if (mode == "restore" || mode == "throw-restore" || mode == "copy-restore")
        return CUDA_ERROR_INVALID_CONTEXT;
    current = saved;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuDeviceGet(CUdevice* out, int v) {
    *out = v;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuDevicePrimaryCtxGetState(CUdevice,
                                                       unsigned int* flags,
                                                       int* active) {
    *flags = 0;
    *active = 1;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuPointerGetAttribute(void* out,
                                                  CUpointer_attribute attr,
                                                  CUdeviceptr ptr) {
    if (attr == CU_POINTER_ATTRIBUTE_MEMORY_TYPE)
        *static_cast<unsigned*>(out) =
            ptr == reinterpret_cast<CUdeviceptr>(target.data())
                ? CU_MEMORYTYPE_DEVICE
                : CU_MEMORYTYPE_HOST;
    else
        *static_cast<int*>(out) = 2;
    return CUDA_SUCCESS;
}
extern "C" cudaError_t CUDARTAPI cudaGetDevice(int* out) {
    *out = device;
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaSetDevice(int v) {
    device = v;
    current = reinterpret_cast<CUcontext>(v + 0x200);
    return mode == "set" ? cudaErrorInvalidDevice : cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaGetDeviceCount(int* out) {
    *out = 8;
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaGetLastError() { return cudaSuccess; }
extern "C" cudaError_t CUDARTAPI cudaMalloc(void**, size_t) {
    return cudaErrorNotSupported;
}
extern "C" cudaError_t CUDARTAPI cudaFree(void*) {
    return cudaErrorNotSupported;
}
extern "C" cudaError_t CUDARTAPI cudaDeviceSynchronize() { return cudaSuccess; }
extern "C" const char* CUDARTAPI cudaGetErrorString(cudaError_t) {
    return "controlled error";
}
extern "C" cudaError_t CUDARTAPI cudaMemcpyAsync(void* dst, const void* src,
                                                 size_t n, cudaMemcpyKind,
                                                 cudaStream_t) {
    if (mode == "copy" || mode == "copy-restore") return cudaErrorInvalidValue;
    std::memcpy(dst, src, n);
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaStreamSynchronize(cudaStream_t) {
    return mode == "sync" ? cudaErrorUnknown : cudaSuccess;
}
namespace mooncake::tent {
Status Platform::synchronizeDevices(const Topology*) { return Status::OK(); }
std::vector<int> Platform::topologyDeviceIndices(const Topology*,
                                                 Topology::MemType) {
    return {};
}
CUDAStreamHandle::~CUDAStreamHandle() {}
CUDAStreamPool::DevicePool::~DevicePool() {}
Status CudaPlatform::getStreamFromPool(CUDAStreamHandle&, int) {
    if (mode == "throw" || mode == "throw-restore")
        throw std::runtime_error("controlled stream exception");
    return mode == "stream" ? Status::CudaError("controlled stream error")
                            : Status::OK();
}
int CudaPlatform::getPointerDeviceId(void* ptr) {
    return getCudaDeviceForPtr(ptr);
}
Status CudaPlatform::probe(std::vector<Topology::NicEntry>&,
                           std::vector<Topology::MemEntry>&) {
    return Status::OK();
}
MemoryType CudaPlatform::getMemoryType(void*) { return MTYPE_UNKNOWN; }
const std::vector<RangeLocation> CudaPlatform::getLocation(void*, size_t,
                                                           bool) {
    return {};
}
}  // namespace mooncake::tent
int main() {
    unsigned errors = 0;
    for (const char* name :
         {"normal", "null", "query", "set", "stream", "copy", "sync", "restore",
          "copy-restore", "throw", "throw-restore"}) {
        mode = name;
        current = mode == "null" ? nullptr : reinterpret_cast<CUcontext>(0x777);
        auto saved = current;
        device = 7;
        restores = 0;
        source.fill(17);
        target.fill(0x5a);
        mooncake::tent::CudaPlatform platform(nullptr);
        bool ok = false, thrown = false;
        bool error_preserved = true;
        try {
            auto status =
                platform.copy(target.data(), source.data(), source.size());
            ok = status.ok();
            if (mode == "restore")
                error_preserved =
                    !status.ok() &&
                    status.ToString().find("cuCtxSetCurrent failed") !=
                        std::string::npos;
            if (mode == "copy-restore")
                error_preserved =
                    status.ToString().find("cuCtxSetCurrent failed") !=
                        std::string::npos &&
                    status.ToString().find("cudaMemcpyAsync") !=
                        std::string::npos;
        } catch (const std::runtime_error& error) {
            thrown = true;
            if (mode == "throw")
                error_preserved =
                    std::string(error.what()) == "controlled stream exception";
            if (mode == "throw-restore") {
                error_preserved =
                    std::string(error.what()).find("cuCtxSetCurrent failed") !=
                    std::string::npos;
                try {
                    std::rethrow_if_nested(error);
                    error_preserved = false;
                } catch (const std::runtime_error& nested) {
                    error_preserved =
                        error_preserved && std::string(nested.what()) ==
                                               "controlled stream exception";
                }
            }
        }
        bool expected_ok = mode == "normal" || mode == "null";
        bool matched = (mode == "restore" || mode == "copy-restore" ||
                        mode == "throw-restore" || current == saved) &&
                       ok == expected_ok &&
                       thrown == (mode == "throw" || mode == "throw-restore") &&
                       error_preserved && (mode == "null" || device == 7) &&
                       restores == (mode == "query" ? 0u : 1u);
        if (expected_ok) matched = matched && target == source;
        if (!matched) ++errors;
        std::cout << name << " ok=" << ok << " restored=" << (current == saved)
                  << " throw=" << thrown << " matched=" << matched << '\n';
    }
    return errors ? 1 : 0;
}
