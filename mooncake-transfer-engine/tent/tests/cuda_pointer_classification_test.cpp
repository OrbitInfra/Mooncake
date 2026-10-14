// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0.
// Software API controls use valid CPU buffers and do not execute CUDA DMA.
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <cuda.h>
#include <cuda_runtime.h>
#include "tent/platform/cuda.h"
#include "tent/platform/cuda_utils.h"
#include "tent/runtime/memory_prober.h"
namespace {
std::string mode;
std::array<unsigned char, 64> source{}, target{};
unsigned copies = 0, device_frees = 0, host_frees = 0;
}  // namespace
extern "C" CUresult CUDAAPI cuInit(unsigned int) {
    return mode == "driver" ? CUDA_ERROR_UNKNOWN : CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuPointerGetAttributes(unsigned int n,
                                                   CUpointer_attribute* attrs,
                                                   void** out,
                                                   CUdeviceptr ptr) {
    if (n != 2 || attrs[0] != CU_POINTER_ATTRIBUTE_MEMORY_TYPE ||
        attrs[1] != CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL)
        return CUDA_ERROR_INVALID_VALUE;
    const bool device = ptr == reinterpret_cast<CUdeviceptr>(target.data());
    if (device && (mode == "query" || mode == "ordinal"))
        return mode == "query" ? CUDA_ERROR_UNKNOWN : CUDA_ERROR_INVALID_DEVICE;
    *static_cast<unsigned*>(out[0]) =
        !device || mode == "host"   ? 0
        : mode == "registered-host" ? CU_MEMORYTYPE_HOST
        : mode == "managed"         ? CU_MEMORYTYPE_UNIFIED
        : mode == "unknown"         ? CU_MEMORYTYPE_ARRAY
                                    : CU_MEMORYTYPE_DEVICE;
    *static_cast<int*>(out[1]) = mode == "invalid-ordinal" ? -1 : 2;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuPointerGetAttribute(void* out,
                                                  CUpointer_attribute attr,
                                                  CUdeviceptr ptr) {
    CUpointer_attribute attrs[] = {CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
                                   CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL};
    unsigned type = 0;
    int device = -1;
    void* outputs[] = {&type, &device};
    auto result = cuPointerGetAttributes(2, attrs, outputs, ptr);
    if (result != CUDA_SUCCESS) return result;
    if (attr == CU_POINTER_ATTRIBUTE_MEMORY_TYPE)
        *static_cast<unsigned*>(out) = type;
    else
        *static_cast<int*>(out) = device;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuCtxGetCurrent(CUcontext* out) {
    *out = nullptr;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuDeviceGet(CUdevice* out, int index) {
    *out = index;
    return CUDA_SUCCESS;
}
extern "C" CUresult CUDAAPI cuDevicePrimaryCtxGetState(CUdevice,
                                                       unsigned int* flags,
                                                       int* active) {
    *flags = 0;
    *active = 0;
    return CUDA_SUCCESS;
}
extern "C" cudaError_t CUDARTAPI cudaGetDevice(int* out) {
    *out = 2;
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaSetDevice(int) { return cudaSuccess; }
extern "C" cudaError_t CUDARTAPI cudaGetDeviceCount(int* out) {
    *out = 8;
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaGetLastError() { return cudaSuccess; }
extern "C" const char* CUDARTAPI cudaGetErrorString(cudaError_t) {
    return "controlled error";
}
extern "C" cudaError_t CUDARTAPI cudaDeviceSynchronize() { return cudaSuccess; }
extern "C" cudaError_t CUDARTAPI cudaMalloc(void**, size_t) {
    return cudaErrorNotSupported;
}
extern "C" cudaError_t CUDARTAPI cudaFree(void*) {
    ++device_frees;
    return cudaSuccess;
}
extern "C" void numa_free(void*, size_t) { ++host_frees; }
extern "C" cudaError_t CUDARTAPI cudaMemcpyAsync(void* dst, const void* src,
                                                 size_t size, cudaMemcpyKind,
                                                 cudaStream_t) {
    ++copies;
    std::memcpy(dst, src, size);
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaMemcpy(void* dst, const void* src,
                                            size_t size, cudaMemcpyKind) {
    ++copies;
    if (mode == "copy-error") return cudaErrorInvalidValue;
    std::memcpy(dst, src, size);
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaDeviceGetPCIBusId(char* output, int size,
                                                       int) {
    if (size < 8) return cudaErrorInvalidValue;
    std::memcpy(output, "0000:00", 8);
    return cudaSuccess;
}
extern "C" cudaError_t CUDARTAPI cudaStreamSynchronize(cudaStream_t) {
    return cudaSuccess;
}
namespace mooncake::tent {
CUDAStreamHandle::~CUDAStreamHandle() {}
CUDAStreamPool::DevicePool::~DevicePool() {}
Status CudaPlatform::getStreamFromPool(CUDAStreamHandle&, int) {
    return Status::OK();
}
#ifdef BASELINE
int CudaPlatform::getPointerDeviceId(void* ptr) {
    return getCudaDeviceForPtr(ptr);
}
#else
Status CudaPlatform::getPointerDeviceId(void* ptr, int& device) {
    CHECK_CU(getCudaDeviceForPtr(ptr, device));
    return Status::OK();
}
#endif
Status Platform::synchronizeDevices(const Topology*) { return Status::OK(); }
std::vector<int> Platform::topologyDeviceIndices(const Topology*,
                                                 Topology::MemType) {
    return {};
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
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    mode = argv[1];
    source.fill(17);
    target.fill(0x5a);
    const bool error = mode == "driver" || mode == "query" ||
                       mode == "ordinal" || mode == "unknown" ||
                       mode == "invalid-ordinal";
    const bool host = mode == "host" || mode == "registered-host";
    if (argc == 3) {
        auto& prober = mooncake::tent::MemoryProber::Instance();
        auto loaded = prober.loadPlugins(argv[2]);
        if (!loaded.ok()) return 2;
        const auto type = prober.type(target.data());
        auto copied =
            prober.memcpy(target.data(), source.data(), source.size());
        const bool rejected = error || mode == "copy-error";
        bool matched = copied.ok() != rejected &&
                       type == (error  ? "*"
                                : host ? "cpu"
                                       : "cuda") &&
                       copies == (error || host ? 0u : 1u);
        if (rejected) {
            for (auto byte : target) matched = matched && byte == 0x5a;
        } else
            matched = matched && target == source;
        std::cout << mode << " plugin=" << copied.ToString()
                  << " copies=" << copies << " type=" << type
                  << " matched=" << matched << '\n';
        prober.unloadPlugins();
        return matched ? 0 : 1;
    }
    mooncake::tent::CudaPlatform platform(nullptr);
    auto result = platform.copy(target.data(), source.data(), source.size());
    bool matched = result.ok() != error &&
                   copies == (error || host ? 0u : 1u) &&
                   target == (error ? std::array<unsigned char, 64>{} : source);
    if (error) {
        matched = !result.ok() && copies == 0;
        for (auto byte : target) matched = matched && byte == 0x5a;
    }
    auto freed = platform.free(target.data(), target.size());
    matched = matched && freed.ok() != error &&
              device_frees == (error || host ? 0u : 1u) &&
              host_frees == (host ? 1u : 0u);
    std::cout << mode << " copy=" << result.ToString()
              << " free=" << freed.ToString() << " copies=" << copies
              << " device_free=" << device_frees << " host_free=" << host_frees
              << " matched=" << matched << '\n';
    return matched ? 0 : 1;
}
