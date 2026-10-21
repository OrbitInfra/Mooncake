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

#include "tent/transport/rdma/buffers.h"
#include "tent/transport/rdma/context.h"

#include <algorithm>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace mooncake {
namespace tent {

// MR warm-up is only beneficial for large buffers (>4GB) where the overhead
// of temp registration is amortized by faster actual registration.
const size_t kMrWarmupMinBytes = 4ull * 1024 * 1024 * 1024;

static unsigned pickMrWarmupThreads(unsigned hwc) {
    constexpr unsigned kMrWarmupMaxThreads = 8;
    constexpr unsigned kMrWarmupMaxThreadsHighCore = 16;
    constexpr unsigned kHighCoreCountThreshold = 64;
    if (hwc == 0) hwc = 1;
    if (hwc > kHighCoreCountThreshold) return kMrWarmupMaxThreadsHighCore;
    return std::min(hwc, kMrWarmupMaxThreads);
}

int warmupMrRegistrationParallel(RdmaContext* context, void* addr,
                                 size_t length) {
    if (!context || length == 0) return 0;
    unsigned hwc = std::thread::hardware_concurrency();
    unsigned num_threads = pickMrWarmupThreads(hwc);
    if (num_threads == 0) return 0;
    if (num_threads == 1) {
        return context->warmupMrRegistration(addr, length);
    }
    size_t chunk_size = (length + num_threads - 1) / num_threads;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    std::vector<int> thread_results(num_threads, 0);

    for (unsigned thread_i = 0; thread_i < num_threads; ++thread_i) {
        size_t offset = thread_i * chunk_size;
        if (offset >= length) break;
        size_t block_len = std::min(chunk_size, length - offset);
        void* block_addr = static_cast<char*>(addr) + offset;
        threads.emplace_back(
            [context, thread_i, block_addr, block_len, &thread_results]() {
                thread_results[thread_i] =
                    context->warmupMrRegistration(block_addr, block_len);
            });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (auto rc : thread_results) {
        if (rc != 0) return rc;
    }
    return 0;
}

LocalBufferManager::LocalBufferManager() {}

LocalBufferManager::~LocalBufferManager() { clear(); }

static inline int getAccessFlags(Permission perm,
                                 bool relaxed_ordering = false) {
    int access = IBV_ACCESS_LOCAL_WRITE;
    if (perm == kGlobalReadWrite) {
        access |= IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    } else if (perm == kGlobalReadOnly) {
        access |= IBV_ACCESS_REMOTE_READ;
    }
    if (relaxed_ordering) {
        access |= IBV_ACCESS_RELAXED_ORDERING;
    }
    return access;
}

Status LocalBufferManager::addBuffer(BufferDesc& desc,
                                     const MemoryOptions& options) {
    return addBufferInternal(desc, options, false);
}

Status LocalBufferManager::addBufferInternal(BufferDesc& desc,
                                             const MemoryOptions& options,
                                             bool force_sequential) {
    const AddressRange range((void*)desc.addr, desc.length);
    const auto contexts = context_list_;
    BufferEntryForRdma staging;
    staging.options = options;
    staging.mem_reg_map.reserve(contexts.size());
    for (auto* context : contexts) {
        if (context) staging.mem_reg_map.emplace(context, nullptr);
    }
    const auto context_count = staging.mem_reg_map.size();
    std::vector<RdmaContext::MemReg> registrations(contexts.size(), nullptr);
    std::vector<std::future<void>> tasks;
    tasks.reserve(context_count);
    desc.lkey.assign(contexts.size(), 0);
    desc.rkey.assign(contexts.size(), 0);
    desc.transports.reserve(desc.transports.size() + 1);
    {
        RWSpinlock::WriteGuard guard(lock_);
        if (!buffer_list_.emplace(range, std::move(staging)).second)
            return Status::RdmaError("RDMA buffer is already owned" LOC_MARK);
    }

    Status failure;
    const auto register_context = [&](size_t id) {
        auto* context = contexts[id];
        const int access = getAccessFlags(
            options.perm, context->isRelaxedOrderingEnabled());
        registrations[id] = context->registerMemReg(
            (void*)desc.addr, desc.length, access);
    };
    try {
        for (size_t id = 0; id < contexts.size(); ++id) {
            if (!contexts[id]) continue;
            if (!force_sequential && context_count > 1) {
                tasks.emplace_back(std::async(std::launch::async,
                                              register_context, id));
            } else {
                register_context(id);
            }
        }
    } catch (const std::exception& error) {
        failure = Status::RdmaError(error.what());
    }
    for (auto& task : tasks) {
        try {
            task.get();
        } catch (const std::exception& error) {
            if (failure.ok()) failure = Status::RdmaError(error.what());
        }
    }
    {
        RWSpinlock::WriteGuard guard(lock_);
        auto& retained = buffer_list_.at(range).mem_reg_map;
        for (size_t id = 0; id < contexts.size(); ++id) {
            auto* context = contexts[id];
            if (!context) continue;
            if (!registrations[id]) {
                retained.erase(context);
                if (failure.ok())
                    failure = Status::RdmaError(
                        "Unable to register buffer of local memory segment"
                        LOC_MARK);
                continue;
            }
            retained.at(context) = registrations[id];
            const auto keys = context->queryMemRegKey(registrations[id]);
            desc.lkey[id] = keys.first;
            desc.rkey[id] = keys.second;
        }
        if (!context_count && failure.ok())
            failure = Status::RdmaError("No RDMA context for buffer" LOC_MARK);
    }
    if (!failure.ok()) return failure;
    desc.transports.push_back(TransportType::RDMA);
    return Status::OK();
}

Status LocalBufferManager::addBuffer(std::vector<BufferDesc>& desc_list,
                                     const MemoryOptions& options) {
    if (desc_list.empty()) return Status::OK();
    if (desc_list.size() == 1)
        return addBufferInternal(desc_list.front(), options, false);
    std::vector<std::future<Status>> tasks;
    tasks.reserve(desc_list.size());
    Status failure;
    try {
        for (auto& desc : desc_list) {
            tasks.emplace_back(std::async(
                std::launch::async, [this, &desc, options]() {
                    return addBufferInternal(desc, options, true);
                }));
        }
    } catch (const std::exception& error) {
        failure = Status::RdmaError(error.what());
    }
    for (auto& task : tasks) {
        try {
            auto status = task.get();
            if (!status.ok() && failure.ok()) failure = status;
        } catch (const std::exception& error) {
            if (failure.ok()) failure = Status::RdmaError(error.what());
        }
    }
    return failure;
}

bool LocalBufferManager::tracksBuffer(const BufferDesc& desc) {
    RWSpinlock::ReadGuard guard(lock_);
    return buffer_list_.count(AddressRange((void*)desc.addr, desc.length)) != 0;
}

Status LocalBufferManager::removeBuffer(BufferDesc& desc) {
    RWSpinlock::WriteGuard guard(lock_);
    AddressRange range((void*)desc.addr, desc.length);
    auto found = buffer_list_.find(range);
    if (found == buffer_list_.end()) return Status::OK();
    bool failed = false;
    auto& registrations = found->second.mem_reg_map;
    for (auto it = registrations.begin(); it != registrations.end();) {
        if (it->first->unregisterMemReg(it->second)) {
            failed = true;
            ++it;
        } else {
            it = registrations.erase(it);
        }
    }
    if (failed) {
        return Status::RdmaError("Unable to unregister buffer" LOC_MARK);
    }
    desc.lkey.clear();
    desc.rkey.clear();
    buffer_list_.erase(found);
    return Status::OK();
}

Status LocalBufferManager::addDevice(RdmaContext* context) {
    RWSpinlock::WriteGuard guard(lock_);
    assert(topology_ && context);
    int index = topology_->getNicId(context->name());
    if (index < 0) {
        LOG(ERROR) << "Device " << context->name()
                   << " not found in the local segment";
        return Status::DeviceNotFound(
            "Device not found in the local segment" LOC_MARK);
    }

    if (context_list_[index]) {
        LOG(WARNING) << "Device " << context->name()
                     << " already exists in the local segment";
    }
    context_list_[index] = context;
    for (auto& buffer : buffer_list_) {
        auto range = buffer.first;
        auto& options = buffer.second.options;
        auto access =
            getAccessFlags(options.perm, context->isRelaxedOrderingEnabled());
        if (buffer.second.mem_reg_map.count(context)) continue;
        auto mem_reg =
            context->registerMemReg(range.addr, range.length, access);
        if (!mem_reg)
            return Status::RdmaError(
                "Device cannot register memory buffer" LOC_MARK);
        buffer.second.mem_reg_map[context] = mem_reg;
    }
    return Status::OK();
}

Status LocalBufferManager::removeDevice(RdmaContext* context, bool do_unreg) {
    RWSpinlock::WriteGuard guard(lock_);
    assert(topology_ && context);
    auto iter = std::find(context_list_.begin(), context_list_.end(), context);
    if (iter == context_list_.end()) return Status::OK();
    for (auto& buffer : buffer_list_) {
        if (!buffer.second.mem_reg_map.count(context)) continue;
        if (do_unreg)
            context->unregisterMemReg(buffer.second.mem_reg_map[context]);
        buffer.second.mem_reg_map.erase(context);
    }
    *iter = nullptr;
    return Status::OK();
}

Status LocalBufferManager::clear() {
    RWSpinlock::WriteGuard guard(lock_);
    for (auto& buffer : buffer_list_) {
        for (auto& elem : buffer.second.mem_reg_map)
            elem.first->unregisterMemReg(elem.second);
    }
    buffer_list_.clear();
    context_list_.clear();
    return Status::OK();
}

}  // namespace tent
}  // namespace mooncake
