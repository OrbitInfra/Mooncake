// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#ifndef TENT_RUNTIME_FILE_IO_H
#define TENT_RUNTIME_FILE_IO_H

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

#include "tent/runtime/transport.h"
#include "tent/runtime/platform.h"
#include "tent/transfer_engine.h"

namespace mooncake::tent {

// Owns the C file API's admission, native sub-batches and caller tokens. A
// logical timeout never destroys this owner. Transport workers, including a
// CUDA copy that cannot establish quiescence, remain charged to the 32 slots.
class FileIO {
   public:
    FileIO(const tent_file_options_t& options, std::shared_ptr<Transport> uring,
           std::shared_ptr<Transport> gds);
    ~FileIO();
    static bool validOptions(const tent_file_options_t& options);
    int checkAvailable();
    bool hasOwners();
    int open(const char* uri, tent_file_access_t access, int transport,
             tent_file_id_t& id, uint64_t& length);
    int submit(tent_file_id_t file, int opcode, uint64_t offset,
               const tent_file_range_t* ranges, size_t count,
               const tent_file_owner_t& owner, tent_file_batch_t& id);
    int submitv(tent_file_id_t file, int opcode, uint64_t offset,
                const tent_file_element_t* elements, size_t count,
                const tent_file_owner_t& owner, tent_file_batch_t& id);
    int drain(tent_file_batch_t id, uint64_t timeout_ms, bool cancel,
              tent_file_result_t& result);
    int release(tent_file_batch_t id);
    int close(tent_file_id_t id, uint64_t timeout_ms,
              tent_file_result_t& result);

    // No active file handles when replacing. Tests wrap the real transport,
    // injecting lost completion observations without simulating file I/O.
    std::shared_ptr<Transport> replaceUringForTest(
        std::shared_ptr<Transport> replacement);
    // Per-engine GPU-copy shim; installed before opening any file. Real I/O
    // still goes through the selected transport. Batches retain this platform.
    void setPlatformForTest(std::shared_ptr<Platform> platform);
    size_t stagingBytesForTest() const;

   private:
    struct File;
    struct Batch;
    struct StagingBudget;
    int submitElements(tent_file_id_t file, int opcode, uint64_t offset,
                       const tent_file_element_t* elements, size_t count,
                       const tent_file_owner_t& owner, tent_file_batch_t& id,
                       bool stage);
    using Clock = std::chrono::steady_clock;
    static void run(const std::shared_ptr<Batch>& batch) noexcept;
    static int wait(const std::shared_ptr<Batch>& batch,
                    Clock::time_point deadline, bool impose_timeout,
                    tent_file_result_t& result);
    tent_file_options_t options_;
    std::shared_ptr<Transport> uring_, gds_;
    std::shared_ptr<Platform> platform_;
    std::shared_ptr<StagingBudget> staging_budget_;
    std::mutex mutex_;
    uint64_t next_id_{1};
    size_t opening_{0};
    std::map<tent_file_id_t, std::shared_ptr<File>> files_;
    std::map<tent_file_batch_t, std::shared_ptr<Batch>> batches_;
};
}  // namespace mooncake::tent
#endif
