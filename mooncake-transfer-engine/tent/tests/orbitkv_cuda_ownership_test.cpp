// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0.
#include "tent/platform/cuda.h"
#include "tent/platform/cuda_utils.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>

namespace {
CUcontext current_context;
int current_device, copies, device_queries;
const void* device_pointer;
bool fail_pointer, fail_switch, fail_copy, fail_sync, fail_restore;
bool managed_pointer;
int event_fd = -1;
void event(char value) {
    if (event_fd >= 0) (void)write(event_fd, &value, 1);
}
class OrbitKvCudaOwnership : public testing::Test {
   protected:
    void SetUp() override {
        current_context = reinterpret_cast<CUcontext>(0x1234);
        current_device = copies = device_queries = 0;
        device_pointer = nullptr;
        fail_pointer = fail_switch = fail_copy = fail_sync = fail_restore =
            false;
        event_fd = -1;
        managed_pointer = false;
    }
};
}  // namespace

extern "C" {
CUresult __wrap_cuInit(unsigned int) { return CUDA_SUCCESS; }
CUresult __wrap_cuPointerGetAttributes(unsigned int count,
                                       CUpointer_attribute* attrs,
                                       void** values, CUdeviceptr pointer) {
    if (fail_pointer) return CUDA_ERROR_UNKNOWN;
    for (unsigned int i = 0; i < count; ++i) {
        bool device = pointer == reinterpret_cast<CUdeviceptr>(device_pointer);
        if (attrs[i] == CU_POINTER_ATTRIBUTE_MEMORY_TYPE)
            *static_cast<unsigned int*>(values[i]) =
                device ? CU_MEMORYTYPE_DEVICE : 0;
        else if (attrs[i] == CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL)
            *static_cast<int*>(values[i]) = device ? 2 : -1;
        else if (attrs[i] == CU_POINTER_ATTRIBUTE_IS_MANAGED)
            *static_cast<unsigned int*>(values[i]) = managed_pointer ? 1 : 0;
        else
            return CUDA_ERROR_INVALID_VALUE;
    }
    return CUDA_SUCCESS;
}
CUresult __wrap_cuCtxGetCurrent(CUcontext* ctx) {
    *ctx = current_context;
    return CUDA_SUCCESS;
}
CUresult __wrap_cuCtxSetCurrent(CUcontext ctx) {
    if (fail_restore) {
        event('E');
        return CUDA_ERROR_UNKNOWN;
    }
    current_context = ctx;
    return CUDA_SUCCESS;
}
cudaError_t __wrap_cudaGetDevice(int* device) {
    ++device_queries;
    *device = current_device;
    return cudaSuccess;
}
cudaError_t __wrap_cudaGetDeviceCount(int* count) {
    *count = 3;
    return cudaSuccess;
}
cudaError_t __wrap_cudaSetDevice(int device) {
    current_device = device;
    current_context = reinterpret_cast<CUcontext>(0x2000 + device);
    return fail_switch && device == 2 ? cudaErrorUnknown : cudaSuccess;
}
cudaError_t __wrap_cudaStreamCreateWithFlags(cudaStream_t* stream,
                                             unsigned int) {
    *stream = reinterpret_cast<cudaStream_t>(0x999);
    return cudaSuccess;
}
cudaError_t __wrap_cudaStreamDestroy(cudaStream_t) {
    event('D');
    return cudaSuccess;
}
cudaError_t __wrap_cudaMemcpyAsync(void* dst, const void* src, size_t size,
                                   cudaMemcpyKind, cudaStream_t) {
    ++copies;
    if (fail_copy) {
        event('E');
        return cudaErrorUnknown;
    }
    std::memcpy(dst, src, size);
    return cudaSuccess;
}
cudaError_t __wrap_cudaStreamSynchronize(cudaStream_t) {
    if (fail_sync) {
        event('E');
        return cudaErrorUnknown;
    }
    return cudaSuccess;
}
}

using mooncake::tent::Config;
using mooncake::tent::CudaPlatform;

TEST_F(OrbitKvCudaOwnership, RestoresCustomAndAbsentContexts) {
    for (bool bare : {false, true}) {
        CudaPlatform platform(std::make_shared<Config>());
        char source[] = "ownership", destination[sizeof(source)]{};
        device_pointer = destination;
        auto saved = bare ? nullptr : reinterpret_cast<CUcontext>(0x1234);
        current_context = saved;
        current_device = 0;
        ASSERT_TRUE(platform.copy(destination, source, sizeof(source)).ok());
        EXPECT_EQ(std::memcmp(source, destination, sizeof(source)), 0);
        EXPECT_EQ(current_context, saved);
        if (!bare) {
            EXPECT_EQ(current_device, 0);
        }
    }
}

TEST_F(OrbitKvCudaOwnership, NativeGdsRejectsManagedHostAndUnknownPointers) {
    char data[8];
    EXPECT_FALSE(mooncake::tent::nativeGdsDevicePointer(data));
    device_pointer = data;
    EXPECT_TRUE(mooncake::tent::nativeGdsDevicePointer(data));
    managed_pointer = true;
    EXPECT_FALSE(mooncake::tent::nativeGdsDevicePointer(data));
    managed_pointer = false;
    fail_pointer = true;
    EXPECT_FALSE(mooncake::tent::nativeGdsDevicePointer(data));
    EXPECT_EQ(copies, 0);
    EXPECT_EQ(current_context, reinterpret_cast<CUcontext>(0x1234));
}

TEST_F(OrbitKvCudaOwnership, SwitchFailureStillRestoresExactContext) {
    CudaPlatform platform(std::make_shared<Config>());
    char source = 'S', destination = 'D';
    device_pointer = &destination;
    auto saved = current_context;
    fail_switch = true;
    EXPECT_FALSE(platform.copy(&destination, &source, 1).ok());
    EXPECT_EQ(current_context, saved);
    EXPECT_EQ(current_device, 0);
    EXPECT_EQ(destination, 'D');
    EXPECT_EQ(copies, 0);
}

TEST_F(OrbitKvCudaOwnership, PointerFailureCannotFallBackToHostCopyOrFree) {
    CudaPlatform platform(std::make_shared<Config>());
    char source = 'S', destination = 'D';
    fail_pointer = true;
    EXPECT_FALSE(platform.copy(&destination, &source, 1).ok());
    EXPECT_FALSE(platform.free(&destination, 1).ok());
    EXPECT_EQ(destination, 'D');
    EXPECT_EQ(copies, 0);
    EXPECT_EQ(device_queries, 0);
}

TEST_F(OrbitKvCudaOwnership, OversizeAdmissionRejectsBeforeDeviceSwitch) {
    CudaPlatform platform(std::make_shared<Config>());
    char source = 'S', destination = 'D';
    device_pointer = &destination;
    EXPECT_TRUE(platform.copy(&destination, &source, (size_t{1} << 30) + 1)
                    .IsTooManyRequests());
    EXPECT_EQ(copies, 0);
    EXPECT_EQ(device_queries, 0);
}

TEST_F(OrbitKvCudaOwnership, UncertainCopyOrRestoreCannotUnwindOwners) {
    for (int failure = 0; failure < 3; ++failure) {
        SCOPED_TRACE(failure);
        int channel[2];
        ASSERT_EQ(pipe(channel), 0);
        auto pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            close(channel[0]);
            event_fd = channel[1];
            fail_copy = failure == 0;
            fail_sync = failure == 1;
            fail_restore = failure == 2;
            CudaPlatform platform(std::make_shared<Config>());
            char source = 'S', destination = 'D';
            device_pointer = &destination;
            (void)platform.copy(&destination, &source, 1);
            event('R');
            _exit(1);
        }
        close(channel[1]);
        pollfd descriptor{channel[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, 5000);
        EXPECT_EQ(ready, 1);
        if (ready == 1) {
            char marker = 0;
            EXPECT_EQ(read(channel[0], &marker, 1), 1);
            EXPECT_EQ(marker, 'E');
            // A return or destructor after the injected error sends another
            // byte.
            EXPECT_EQ(poll(&descriptor, 1, 200), 0);
        }
        int status = 0;
        EXPECT_EQ(waitpid(pid, &status, WNOHANG), 0);
        kill(pid, SIGKILL);
        EXPECT_EQ(waitpid(pid, &status, 0), pid);
        close(channel[0]);
    }
}
