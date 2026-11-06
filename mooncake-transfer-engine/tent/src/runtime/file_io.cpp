// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include "tent/runtime/file_io.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

#ifdef USE_CUDA
#include "tent/platform/cuda_utils.h"
#endif

namespace mooncake::tent {
namespace {
constexpr size_t kMaxFiles = 256, kMaxBatches = 32, kMaxRanges = 16384;
constexpr uint64_t kMaxBytes = 64ull << 20, kSliceBytes = 16ull << 20;
constexpr size_t kStagingWindow = 4ull << 20, kStagingBudget = 64ull << 20;
constexpr uint64_t kMaxTimeoutMs = 24 * 60 * 60 * 1000;
using Clock = std::chrono::steady_clock;

bool terminal(TransferStatusEnum s) { return s != PENDING && s != INITIAL; }
Clock::time_point deadlineAfter(uint64_t ms) {
    return Clock::now() + std::chrono::milliseconds(ms);
}

bool nativeGdsPointer(const void* pointer) {
#ifdef USE_CUDA
    return nativeGdsDevicePointer(pointer);
#else
    return false;
#endif
}
}  // namespace

struct FileIO::File {
    int fd{-1};
    tent_file_id_t id{0};
    uint64_t length{0};
    tent_file_access_t access;
    bool closing{false};
    bool native_only{false};
    int transport_type;
    size_t depth;
    size_t slice_bytes{kSliceBytes};
    std::shared_ptr<Transport> transport;
    Transport::FileRef context;
    std::mutex close_mutex, join_mutex;
    std::condition_variable closed;
    std::thread closer;
    bool close_started{false}, close_done{false}, close_failed{false};
#ifdef USE_CUDA
    CUcontext cuda_context{nullptr};
#endif
    ~File() {
        context.reset();
        if (fd >= 0) ::close(fd);
    }
};

struct FileIO::StagingBudget {
    std::atomic<size_t> bytes{0};
};

struct FileIO::Batch {
    std::mutex mutex, join_mutex;
    std::condition_variable changed;
    std::thread worker;
    bool started{false}, drained{false}, uncertain{false};
    std::atomic<int> stop{TENT_FILE_OK};
    tent_file_result_t result{TENT_FILE_OK, 0, 0};
    tent_file_id_t file_id;
    std::shared_ptr<File> file;
    std::vector<Request> requests;
    std::vector<tent_file_element_t> elements;
    std::shared_ptr<Platform> platform;
    std::shared_ptr<StagingBudget> budget;
    size_t staging_size{0}, charge{0};
    void* host_staging{nullptr};
    void* io_staging{nullptr};
    BufferDesc staging_desc;
    bool device_staging{false}, registered{false};
    tent_file_owner_t owner;
    Transport::SubBatchRef native{nullptr};
#ifdef USE_CUDA
    CUcontext cuda_context{nullptr};
#endif

    void stopDispatch(int reason) {
        int expected = TENT_FILE_OK;
        stop.compare_exchange_strong(expected, reason);
    }
    void unknown() {
        stopDispatch(TENT_FILE_IO_ERROR);
        std::lock_guard<std::mutex> lock(mutex);
        uncertain = true;
        changed.notify_all();
    }

    // Called on the worker with the captured GDS context current. Raw staging
    // pointers deliberately survive any uncertain copy, deregister or free.
    bool allocateStaging() {
        if (posix_memalign(&host_staging, 4096, staging_size)) return false;
        io_staging = host_staging;
#ifdef USE_CUDA
        if (file->transport_type == TRANSPORT_GDS) {
            CUdevice device;
            CUdeviceptr ptr = 0;
            if (cuCtxGetDevice(&device) != CUDA_SUCCESS ||
                cuMemAlloc(&ptr, staging_size) != CUDA_SUCCESS)
                return false;
            io_staging = reinterpret_cast<void*>(ptr);
            device_staging = true;
            // Never let an unexpected device allocation alignment send the
            // native-only profile through cuFile's compatibility path.
            if (file->native_only && ptr % 4096) return false;
            staging_desc.addr = ptr;
            staging_desc.length = staging_size;
            staging_desc.location = "cuda:" + std::to_string(device);
            MemoryOptions options;
            options.location = staging_desc.location;
            // Even a failed registration may have acquired native ownership.
            if (!file->transport->addMemoryBuffer(staging_desc, options).ok())
                return false;
            registered = true;
        }
#endif
        return true;
    }

    bool copyWindow(uint64_t start, size_t length, bool write) {
        if (write) {
            memset(host_staging, 0, length);
            if (device_staging &&
                !platform->copy(io_staging, host_staging, length).ok())
                return false;
        }
        uint64_t element_start = 0;
        for (const auto& element : elements) {
            const uint64_t end = element_start + element.length;
            const uint64_t lo = std::max(start, element_start);
            const uint64_t hi = std::min(start + length, end);
            if (lo < hi && element.kind == TENT_FILE_ELEMENT_RANGE) {
                auto* caller =
                    static_cast<char*>(element.address) + (lo - element_start);
                auto* scratch = static_cast<char*>(io_staging) + (lo - start);
                auto status = write ? platform->copy(scratch, caller, hi - lo)
                                    : platform->copy(caller, scratch, hi - lo);
                if (!status.ok()) return false;
            }
            element_start = end;
            if (end >= start + length) break;
        }
        return true;
    }

    bool releaseDeviceStaging() {
        if (registered) {
            if (!file->transport->removeMemoryBuffer(staging_desc).ok())
                return false;
            registered = false;
        }
#ifdef USE_CUDA
        if (device_staging) {
            if (cuMemFree(reinterpret_cast<CUdeviceptr>(io_staging)) !=
                CUDA_SUCCESS)
                return false;
            device_staging = false;
            io_staging = nullptr;
        }
#endif
        return true;
    }
};

FileIO::FileIO(const tent_file_options_t& options,
               std::shared_ptr<Transport> uring, std::shared_ptr<Transport> gds)
    : options_(options),
      uring_(std::move(uring)),
      gds_(std::move(gds)),
      platform_(&Platform::getLoader(), [](Platform*) {}),
      staging_budget_(std::make_shared<StagingBudget>()) {}

FileIO::~FileIO() {
    // The C engine destructor refuses destruction while these exist. Native
    // callers must obey the same lifetime rule; never unwind a live worker.
    if (!files_.empty() || !batches_.empty()) std::terminate();
}

bool FileIO::validOptions(const tent_file_options_t& o) {
    auto depth = [](uint32_t v) { return v >= 1 && v <= 1024; };
    return o.struct_size == sizeof(o) && o.io_uring_enabled <= 1 &&
           o.gds_mode >= TENT_GDS_OFF && o.gds_mode <= TENT_GDS_NATIVE_ONLY &&
           (!o.io_uring_enabled || depth(o.io_uring_queue_depth)) &&
           (o.gds_mode == TENT_GDS_OFF || depth(o.gds_queue_depth));
}

int FileIO::checkAvailable() {
    if (options_.io_uring_enabled && !uring_) return TENT_FILE_UNAVAILABLE;
    if (options_.gds_mode != TENT_GDS_OFF &&
        (!gds_ ||
         !gds_->checkFileMode(options_.gds_mode == TENT_GDS_NATIVE_ONLY).ok()))
        return TENT_FILE_UNAVAILABLE;
    return TENT_FILE_OK;
}

bool FileIO::hasOwners() {
    std::lock_guard<std::mutex> lock(mutex_);
    return opening_ != 0 || !files_.empty() || !batches_.empty();
}

std::shared_ptr<Transport> FileIO::replaceUringForTest(
    std::shared_ptr<Transport> replacement) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!files_.empty()) return nullptr;
    return std::exchange(uring_, std::move(replacement));
}

void FileIO::setPlatformForTest(std::shared_ptr<Platform> platform) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (platform && !opening_ && files_.empty() && batches_.empty())
        platform_ = std::move(platform);
}

size_t FileIO::stagingBytesForTest() const {
    return staging_budget_->bytes.load();
}

int FileIO::open(const char* uri, tent_file_access_t access, int transport,
                 tent_file_id_t& id, uint64_t& length) {
    if (!uri || std::string_view(uri).substr(0, 7) != "file://" || !uri[7] ||
        access < TENT_FILE_READ || access > TENT_FILE_READ_WRITE ||
        (transport != TRANSPORT_IOURING && transport != TRANSPORT_GDS))
        return TENT_FILE_INVALID_ARGUMENT;
    const bool uring = transport == TRANSPORT_IOURING;
    if ((uring && !options_.io_uring_enabled) ||
        (!uring && options_.gds_mode == TENT_GDS_OFF))
        return TENT_FILE_DISABLED;
    auto file = std::make_shared<File>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (files_.size() + opening_ >= kMaxFiles) return TENT_FILE_BUSY;
        ++opening_;
        file->transport = uring ? uring_ : gds_;
    }
    auto release_slot = [this](FileIO*) {
        std::lock_guard<std::mutex> lock(mutex_);
        --opening_;
    };
    std::unique_ptr<FileIO, decltype(release_slot)> opening(this, release_slot);
    if (!file->transport) return TENT_FILE_UNAVAILABLE;
    file->native_only = !uring && options_.gds_mode == TENT_GDS_NATIVE_ONLY;
    if (!file->transport->checkFileMode(file->native_only).ok())
        return TENT_FILE_UNAVAILABLE;
    if (!file->transport->fileSliceSize(file->native_only, file->slice_bytes)
             .ok() ||
        file->slice_bytes == 0 || file->slice_bytes > kSliceBytes)
        return TENT_FILE_UNAVAILABLE;
#ifdef USE_CUDA
    if (!uring && (!ensureCudaDriverInit() ||
                   cuCtxGetCurrent(&file->cuda_context) != CUDA_SUCCESS ||
                   !file->cuda_context))
        return TENT_FILE_UNAVAILABLE;
#endif
    const int flags = access == TENT_FILE_READ    ? O_RDONLY
                      : access == TENT_FILE_WRITE ? O_WRONLY
                                                  : O_RDWR;
    file->fd = ::open(uri + 7, flags | O_CLOEXEC);
    if (file->fd < 0) return TENT_FILE_IO_ERROR;
    struct stat st{};
    if (fstat(file->fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
        return TENT_FILE_INVALID_ARGUMENT;
    if (file->native_only) {
        // Restrict this profile to local ext4/XFS. Other filesystems have
        // cuFile-specific POSIX routing settings beyond allow_compat_mode.
        struct statfs fs{};
        if (fstatfs(file->fd, &fs) ||
            (fs.f_type != 0xEF53 && fs.f_type != 0x58465342))
            return TENT_FILE_UNAVAILABLE;
    }
    auto status = file->transport->openFile(file->fd, file->context);
    if (!status.ok()) return TENT_FILE_IO_ERROR;
    file->length = st.st_size;
    file->access = access;
    file->transport_type = transport;
    file->depth =
        uring ? options_.io_uring_queue_depth : options_.gds_queue_depth;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file->id = next_id_++;
        files_.emplace(file->id, file);
    }
    id = file->id;
    length = file->length;
    return TENT_FILE_OK;
}

int FileIO::submit(tent_file_id_t file_id, int opcode, uint64_t offset,
                   const tent_file_range_t* ranges, size_t count,
                   const tent_file_owner_t& owner, tent_file_batch_t& id) {
    if (!ranges || !count || count > kMaxRanges)
        return TENT_FILE_INVALID_ARGUMENT;
    bool stage = offset % 4096 != 0;
    std::vector<tent_file_element_t> elements;
    elements.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        elements.push_back(
            {TENT_FILE_ELEMENT_RANGE, ranges[i].address, ranges[i].length});
        stage |= reinterpret_cast<uintptr_t>(ranges[i].address) % 4096 ||
                 ranges[i].length % 4096;
    }
    return submitElements(file_id, opcode, offset, elements.data(), count,
                          owner, id, stage);
}

int FileIO::submitv(tent_file_id_t file_id, int opcode, uint64_t offset,
                    const tent_file_element_t* elements, size_t count,
                    const tent_file_owner_t& owner, tent_file_batch_t& id) {
    return submitElements(file_id, opcode, offset, elements, count, owner, id,
                          true);
}

int FileIO::submitElements(tent_file_id_t file_id, int opcode, uint64_t offset,
                           const tent_file_element_t* elements, size_t count,
                           const tent_file_owner_t& owner,
                           tent_file_batch_t& id, bool stage) {
    if (!elements || !count || count > kMaxRanges || !owner.retain ||
        !owner.release || (opcode != OPCODE_READ && opcode != OPCODE_WRITE))
        return TENT_FILE_INVALID_ARGUMENT;
    std::shared_ptr<File> file;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(file_id);
        if (it == files_.end()) return TENT_FILE_INVALID_ARGUMENT;
        file = it->second;
        if (file->closing || batches_.size() >= kMaxBatches)
            return TENT_FILE_BUSY;
    }
    if (!(file->access &
          (opcode == OPCODE_READ ? TENT_FILE_READ : TENT_FILE_WRITE)))
        return TENT_FILE_PERMISSION;
    auto batch = std::make_shared<Batch>();
    batch->file = file;
    batch->file_id = file_id;
    batch->owner = owner;
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& range = elements[i];
        const uintptr_t address = reinterpret_cast<uintptr_t>(range.address);
        const bool padding = range.kind == TENT_FILE_ELEMENT_PADDING;
        if ((range.kind != TENT_FILE_ELEMENT_RANGE && !padding) ||
            (padding ? address != 0 : !address) || !range.length ||
            range.length > kMaxBytes - total ||
            (!padding && range.length > UINTPTR_MAX - address))
            return TENT_FILE_INVALID_ARGUMENT;
        if (file->native_only && !padding && !nativeGdsPointer(range.address))
            return TENT_FILE_INVALID_ARGUMENT;
        total += range.length;
    }
    if (offset > file->length || total > file->length - offset ||
        (file->native_only && (offset % 4096 || total % 4096)))
        return TENT_FILE_INVALID_ARGUMENT;
    if (stage) {
        batch->elements.assign(elements, elements + count);
        batch->staging_size =
            std::min({size_t{total}, kStagingWindow, file->slice_bytes});
        batch->charge = batch->staging_size *
                        (file->transport_type == TRANSPORT_GDS ? 2 : 1);
        batch->budget = staging_budget_;
        for (uint64_t part = 0; part < total; part += batch->staging_size) {
            Request req;
            req.opcode = opcode == OPCODE_READ ? Request::READ : Request::WRITE;
            req.source = nullptr;  // worker-owned scratch, allocated once
            req.length = std::min(uint64_t{batch->staging_size}, total - part);
            req.target_offset = offset + part;
            req.target_id = LOCAL_SEGMENT_ID;
            batch->requests.push_back(std::move(req));
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            const auto& range = elements[i];
            const uintptr_t address =
                reinterpret_cast<uintptr_t>(range.address);
            for (uint64_t part = 0; part < range.length;
                 part += file->slice_bytes) {
                Request req;
                req.opcode =
                    opcode == OPCODE_READ ? Request::READ : Request::WRITE;
                req.source = reinterpret_cast<void*>(address + part);
                req.length =
                    std::min(uint64_t{file->slice_bytes}, range.length - part);
                req.target_offset = offset;
                req.target_id =
                    LOCAL_SEGMENT_ID;  // explicit sub-batch file owner
                batch->requests.push_back(std::move(req));
                offset += batch->requests.back().length;
            }
        }
    }
#ifdef USE_CUDA
    if (file->transport_type == TRANSPORT_GDS) {
        if (!ensureCudaDriverInit() ||
            cuCtxGetCurrent(&batch->cuda_context) != CUDA_SUCCESS ||
            !batch->cuda_context)
            return TENT_FILE_UNAVAILABLE;
    }
#endif
    // Native pointer/context queries above must not hold the registry lock:
    // a blocked driver must not prevent other batches from timing out/draining.
    std::lock_guard<std::mutex> lock(mutex_);
    if (file->closing || batches_.size() >= kMaxBatches) return TENT_FILE_BUSY;
    if (batch->charge > kStagingBudget - staging_budget_->bytes.load())
        return TENT_FILE_BUSY;
    batch->platform = platform_;
    const auto batch_id = next_id_++;
    batches_.emplace(batch_id, batch);
    staging_budget_->bytes.fetch_add(batch->charge);
    try {
        batch->worker = std::thread(run, batch);
    } catch (...) {
        staging_budget_->bytes.fetch_sub(batch->charge);
        batches_.erase(batch_id);
        return TENT_FILE_BUSY;
    }
    owner.retain(owner.context);
    {
        std::lock_guard<std::mutex> guard(batch->mutex);
        batch->started = true;
        batch->changed.notify_all();
    }
    id = batch_id;
    return TENT_FILE_OK;
}

void FileIO::run(const std::shared_ptr<Batch>& batch) noexcept {
    {
        std::unique_lock<std::mutex> lock(batch->mutex);
        batch->changed.wait(lock, [&] { return batch->started; });
    }
    int outcome = TENT_FILE_OK;
    uint64_t bytes = 0;
    try {
        auto& file = *batch->file;
        auto& transport = *file.transport;
#ifdef USE_CUDA
        if (batch->cuda_context &&
            cuCtxSetCurrent(batch->cuda_context) != CUDA_SUCCESS) {
            outcome = TENT_FILE_IO_ERROR;
        }
#endif
        if (outcome == TENT_FILE_OK && batch->staging_size &&
            !batch->allocateStaging()) {
            if (batch->host_staging) {
                batch->unknown();
                return;
            }
            // Host allocation failed before any copy, registration or I/O.
            outcome = TENT_FILE_IO_ERROR;
        }
        for (size_t base = 0; base < batch->requests.size() &&
                              outcome == TENT_FILE_OK && !batch->stop.load();) {
            const size_t count =
                std::min(batch->staging_size ? size_t{1} : file.depth,
                         batch->requests.size() - base);
            std::vector<Request> window(batch->requests.begin() + base,
                                        batch->requests.begin() + base + count);
            std::vector<TransferStatus> statuses(count, {PENDING, 0});
            if (batch->staging_size) {
                window[0].source = batch->io_staging;
                if (window[0].opcode == Request::WRITE &&
                    !batch->copyWindow(bytes, window[0].length, true)) {
                    batch->unknown();
                    return;
                }
                // Cancellation during a staging copy closes future dispatch.
                if (batch->stop.load()) break;
            }
            auto status = transport.allocateSubBatch(batch->native, count);
            if (!status.ok()) {
                outcome = TENT_FILE_IO_ERROR;
                break;
            }
            batch->native->file = file.context;
            // This is the only dispatch. Poll failures never replay a window.
            status = transport.submitTransferTasks(batch->native, window);
            if (!status.ok()) outcome = TENT_FILE_IO_ERROR;
            const size_t admitted = batch->native->size();
            if (admitted > count) {
                batch->unknown();
                return;
            }
            if (admitted != count) outcome = TENT_FILE_IO_ERROR;
            for (;;) {
                bool done = true;
                for (size_t i = 0; i < admitted; ++i) {
                    if (!terminal(statuses[i].s)) {
                        status = transport.getTransferStatus(batch->native, i,
                                                             statuses[i]);
                        if (!status.ok()) {
                            // Retain all native/caller owners. Re-observing
                            // completion is safe; resubmitting is not.
                            batch->unknown();
                            outcome = TENT_FILE_IO_ERROR;
                            statuses[i].s = PENDING;
                        }
                    }
                    done &= terminal(statuses[i].s);
                }
                if (done) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (size_t i = 0; i < admitted; ++i) {
                if (statuses[i].transferred_bytes > window[i].length) {
                    batch->unknown();
                    return;
                }
                if (batch->staging_size && window[i].opcode == Request::READ &&
                    !batch->copyWindow(bytes, statuses[i].transferred_bytes,
                                       false)) {
                    batch->unknown();
                    return;
                }
                bytes += statuses[i].transferred_bytes;
                if (outcome == TENT_FILE_OK) {
                    if (statuses[i].s == CANCELED)
                        outcome = TENT_FILE_CANCELED;
                    else if (statuses[i].s == TIMEOUT)
                        outcome = TENT_FILE_TIMEOUT;
                    else if (transport.fileTaskShortIO(batch->native, i) ||
                             (statuses[i].s == COMPLETED &&
                              statuses[i].transferred_bytes < window[i].length))
                        outcome = TENT_FILE_SHORT_IO;
                    else if (statuses[i].s != COMPLETED)
                        outcome = TENT_FILE_IO_ERROR;
                }
            }
            // #5068's free is reached only with every I/O already reaped.
            // GDS likewise reaches free only after every slice is terminal,
            // never interpreting a quarantine handoff as a release receipt.
            status = transport.freeSubBatch(batch->native);
            if (!status.ok() || batch->native) {
                batch->unknown();
                return;
            }
            base += count;
            {
                std::lock_guard<std::mutex> lock(batch->mutex);
                batch->result.transferred_bytes = bytes;
            }
        }
        if (!batch->releaseDeviceStaging()) {
            batch->unknown();
            return;
        }
#ifdef USE_CUDA
        if (batch->cuda_context && cuCtxSetCurrent(nullptr) != CUDA_SUCCESS) {
            batch->unknown();
            return;
        }
#endif
        if (outcome == TENT_FILE_OK) outcome = batch->stop.load();
        free(batch->host_staging);
        batch->host_staging = nullptr;
        batch->io_staging = nullptr;
        if (batch->budget) batch->budget->bytes.fetch_sub(batch->charge);
        batch->charge = 0;
        batch->file.reset();
        batch->owner.release(batch->owner.context);
        std::lock_guard<std::mutex> lock(batch->mutex);
        batch->result = {static_cast<tent_file_error_t>(outcome), 1, bytes};
        batch->drained = true;
        batch->changed.notify_all();
    } catch (...) {
        // Even an allocation or provider exception cannot release an owner
        // after possible dispatch. The engine retains this batch indefinitely.
        batch->unknown();
    }
}

int FileIO::wait(const std::shared_ptr<Batch>& batch,
                 Clock::time_point deadline, bool impose_timeout,
                 tent_file_result_t& result) {
    std::unique_lock<std::mutex> lock(batch->mutex);
    batch->changed.wait_until(
        lock, deadline, [&] { return batch->drained || batch->uncertain; });
    if (!batch->drained) {
        if (impose_timeout) batch->stopDispatch(TENT_FILE_TIMEOUT);
        result = batch->result;
        result.outcome =
            batch->uncertain
                ? TENT_FILE_IO_ERROR
                : static_cast<tent_file_error_t>(batch->stop.load());
        return TENT_FILE_UNQUIESCED;
    }
    result = batch->result;
    lock.unlock();
    std::lock_guard<std::mutex> joining(batch->join_mutex);
    if (batch->worker.joinable()) batch->worker.join();
    return result.outcome;
}

int FileIO::drain(tent_file_batch_t id, uint64_t timeout_ms, bool cancel,
                  tent_file_result_t& result) {
    if (timeout_ms > kMaxTimeoutMs) return TENT_FILE_INVALID_ARGUMENT;
    const auto deadline = deadlineAfter(timeout_ms);
    std::shared_ptr<Batch> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = batches_.find(id);
        if (it == batches_.end()) return TENT_FILE_INVALID_ARGUMENT;
        batch = it->second;
    }
    if (cancel) batch->stopDispatch(TENT_FILE_CANCELED);
    return wait(batch, deadline, timeout_ms != 0, result);
}

int FileIO::release(tent_file_batch_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = batches_.find(id);
    if (it == batches_.end()) return TENT_FILE_INVALID_ARGUMENT;
    tent_file_result_t result{};
    wait(it->second, Clock::now(), false, result);
    if (!result.drained) return TENT_FILE_UNQUIESCED;
    batches_.erase(it);
    return TENT_FILE_OK;
}

int FileIO::close(tent_file_id_t id, uint64_t timeout_ms,
                  tent_file_result_t& result) {
    if (timeout_ms > kMaxTimeoutMs) return TENT_FILE_INVALID_ARGUMENT;
    const auto deadline = deadlineAfter(timeout_ms);
    std::vector<std::shared_ptr<Batch>> batches;
    std::shared_ptr<File> file;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(id);
        if (it == files_.end()) return TENT_FILE_INVALID_ARGUMENT;
        file = it->second;
        file->closing = true;
        for (auto& [_, batch] : batches_) {
            if (batch->file_id != id) continue;
            batch->stopDispatch(TENT_FILE_CANCELED);
            batches.push_back(batch);
        }
    }
    bool drained = true;
    for (auto& batch : batches) {
        tent_file_result_t receipt{};
        wait(batch, deadline, false, receipt);
        drained &= receipt.drained != 0;
    }
    if (!drained) {
        result = {TENT_FILE_UNQUIESCED, 0, 0};
        return TENT_FILE_UNQUIESCED;
    }
    std::unique_lock<std::mutex> closing(file->close_mutex);
    if (!file->close_started) {
        file->closer = std::thread([file] {
            bool ok = false;
            try {
#ifdef USE_CUDA
                if (file->cuda_context &&
                    cuCtxSetCurrent(file->cuda_context) != CUDA_SUCCESS) {
                    std::lock_guard<std::mutex> guard(file->close_mutex);
                    file->close_failed = true;
                    file->closed.notify_all();
                    return;
                }
#endif
                ok = file->transport->closeFile(file->context).ok() &&
                     !file->context;
                if (ok && file->fd >= 0) {
                    ::close(file->fd);
                    file->fd = -1;
                }
#ifdef USE_CUDA
                if (file->cuda_context &&
                    cuCtxSetCurrent(nullptr) != CUDA_SUCCESS)
                    ok = false;
#endif
            } catch (...) { /* unknown cleanup effect: never retry */
            }
            std::lock_guard<std::mutex> guard(file->close_mutex);
            file->close_done = ok;
            file->close_failed = !ok;
            file->closed.notify_all();
        });
        file->close_started = true;
    }
    file->closed.wait_until(closing, deadline, [&] {
        return file->close_done || file->close_failed;
    });
    if (!file->close_done) {
        result = {TENT_FILE_UNQUIESCED, 0, 0};
        return TENT_FILE_UNQUIESCED;
    }
    closing.unlock();
    {
        std::lock_guard<std::mutex> joining(file->join_mutex);
        if (file->closer.joinable()) file->closer.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        files_.erase(id);
    }
    file.reset();
    result = {TENT_FILE_OK, 1, 0};
    return TENT_FILE_OK;
}
}  // namespace mooncake::tent
