// Copyright 2024 KVCache.AI
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

#ifndef TENT_API_H
#define TENT_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define tent_batch_id_t uint64_t
#define tent_segment_id_t uint64_t

#ifndef LOCAL_SEGMENT_ID
#define LOCAL_SEGMENT_ID (0ull)
#endif

#define OPCODE_READ (0)
#define OPCODE_WRITE (1)

/* IMPORTANT: callers MUST zero-initialize this struct
 * (e.g. `tent_request_t r = {0};`). transport_hint relies on
 * UNSPEC == 0 for the no-hint default. */
struct tent_request {
    int opcode;
    void* source;
    tent_segment_id_t target_id;
    uint64_t target_offset;
    uint64_t length;
    int priority;       /* Request priority (0=HIGH, 1=MEDIUM, 2=LOW) */
    int transport_hint; /* TRANSPORT_UNSPEC=follow policy, else pin transport */
    char expected_source_epoch[33]; /* Empty for unbound native operations. */
};

typedef struct tent_request tent_request_t;

#define STATUS_WAITING (0)
#define STATUS_PENDING (1)
#define STATUS_INVALID (2)
#define STATUS_CANCELED (3)
#define STATUS_COMPLETED (4)
#define STATUS_TIMEOUT (5)
#define STATUS_FAILED (6)

struct tent_status {
    int status;
    uint64_t transferred_bytes;
};

typedef struct tent_status tent_status_t;

typedef void* tent_engine_t;

/* Generic int-returning operations: 0 on success, otherwise the negative
 * native TENT Status::Code (not errno). Invalid arguments return -1;
 * caught exceptions/wrapper allocation failures return -199. Exceptions to
 * this convention: tent_available returns a boolean; tent_close_source has
 * its documented drain result; the file API uses positive TENT_FILE_* codes.
 * Handle/pointer/void-returning functions retain their existing contracts.
 * A failure is not a physical drain receipt. A code alone does not authorize
 * replay of an entire batch: earlier accepted tasks may have taken effect. */
#define TENT_ERROR_TOO_MANY_REQUESTS 2
#define TENT_ERROR_RPC_CONNECTION 134
#define TENT_ERROR_RPC_UNKNOWN_EFFECT 135

/* A connection error is before RPC dispatch; unknown effect means the request
 * may have reached the peer. Never replay an unknown-effect WRITE. */

#define TENT_ALL_TASKS SIZE_MAX

/* Query a cached terminal task, or the whole batch with TENT_ALL_TASKS.
 * Returns 0 when terminal, -2 while pending, -1 for an invalid handle/index.
 * On success, status_code is POSITIVE native code (0 for COMPLETED) and
 * effect_known is 1 only with an outcome receipt. Missing failure evidence,
 * timeout or cancellation conservatively reports 135/0. Other failures keep
 * their native code even when effect_known is 0. This query does not progress,
 * dispatch, retry or release anything; poll with tent_task/overall_status.
 * Batch: all tasks must be terminal; 135 takes precedence, otherwise the
 * first nonzero code in task order wins; effect_known is the AND of all tasks.
 * A known batch outcome is not permission to replay its successful tasks.
 * On query error the outputs are initialized to 135/0. Receipts remain valid
 * until tent_free_batch. Neither a terminal state nor effect_known proves
 * physical quiescence or permits releasing registrations/buffers. */
int tent_terminal_result(tent_engine_t engine, tent_batch_id_t batch_id,
                         size_t task_id, int* status_code, int* effect_known);

/* OrbitKV file API. Additive; independent of the generic logical batch API. */
typedef enum tent_file_error {
    TENT_FILE_OK = 0,
    TENT_FILE_INVALID_ARGUMENT = 1,
    TENT_FILE_DISABLED = 2,
    TENT_FILE_UNAVAILABLE = 3,
    TENT_FILE_IO_ERROR = 4,
    TENT_FILE_SHORT_IO = 5,
    TENT_FILE_CANCELED = 6,
    TENT_FILE_TIMEOUT = 7,
    TENT_FILE_UNQUIESCED = 8,
    TENT_FILE_BUSY = 9,
    TENT_FILE_PERMISSION = 10
} tent_file_error_t;

typedef enum tent_gds_mode {
    TENT_GDS_OFF = 0,
    TENT_GDS_ON = 1,
    TENT_GDS_NATIVE_ONLY = 2
} tent_gds_mode_t;

typedef enum tent_file_access {
    TENT_FILE_READ = 1,
    TENT_FILE_WRITE = 2,
    TENT_FILE_READ_WRITE = 3
} tent_file_access_t;

typedef struct tent_file_options {
    uint32_t struct_size;      /* sizeof(tent_file_options_t) */
    uint32_t io_uring_enabled; /* 0 or 1 */
    tent_gds_mode_t gds_mode;
    uint32_t io_uring_queue_depth; /* 1..1024 when enabled */
    uint32_t gds_queue_depth;      /* 1..1024 when enabled */
} tent_file_options_t;

typedef uint64_t tent_file_id_t;
typedef uint64_t tent_file_batch_t;

typedef struct tent_file_range {
    void* address;
    uint64_t length;
} tent_file_range_t;

/* Additive vector API; tent_file_range_t and tent_file_submit are unchanged. */
typedef enum tent_file_element_kind {
    TENT_FILE_ELEMENT_RANGE = 0,
    TENT_FILE_ELEMENT_PADDING = 1
} tent_file_element_kind_t;

typedef struct tent_file_element {
    tent_file_element_kind_t kind;
    void* address;   /* RANGE: nonnull; PADDING: must be NULL */
    uint64_t length; /* nonzero; padding counts toward the file extent */
} tent_file_element_t;

/* Both callbacks must be nonthrowing, nonblocking, and must not reenter this
 * file API. retain/release must own the actual backing, registrations and
 * extent. Producer readiness and exclusive READ destinations are caller
 * obligations. */
typedef struct tent_file_owner {
    void* context;
    void (*retain)(void* context);
    void (*release)(void* context);
} tent_file_owner_t;

typedef struct tent_file_result {
    tent_file_error_t outcome;
    uint32_t drained; /* 1 only after native I/O, staging and owner release */
    uint64_t transferred_bytes; /* submitv: prefix including padding;
                                 * legacy aligned submit: sum over ranges */
} tent_file_result_t;

/* Applies the existing thread-local settings, then overrides file transport
 * enable/depth settings. Requested but unavailable transports fail creation. */
int tent_create_engine_with_files(const tent_file_options_t* options,
                                  tent_engine_t* engine);

/* Existing regular files only; pins the inode. transport is IOURING or GDS.
 * The returned handle belongs only to this API, not tent_open_segment(). */
int tent_file_open(tent_engine_t engine, const char* uri,
                   tent_file_access_t access, int transport,
                   tent_file_id_t* file, uint64_t* length);

/* Up to 16384 ranges / 64 MiB, mapped consecutively from file_offset.
 * Unaligned vectors use the same bounded staging as submitv (without padding).
 * Aligned vectors retain parallel windows and sum-of-ranges byte receipts.
 * READ/WRITE use OPCODE_READ/WRITE. No extension, replay, fsync or atomicity.
 * On success, retain has run and one batch owns the token until drained.
 * On rejection, no I/O or callback has run and *batch is zero. */
int tent_file_submit(tent_engine_t engine, tent_file_id_t file, int opcode,
                     uint64_t file_offset, const tent_file_range_t* ranges,
                     size_t count, const tent_file_owner_t* owner,
                     tent_file_batch_t* batch);

/* Contiguous file extent, in element order. WRITE padding emits zeros; READ
 * padding consumes/discards file bytes without touching caller memory. TENT
 * owns bounded staging (4 MiB windows, 64 MiB charged per engine) through all
 * I/O and copies. io_uring accepts odd addresses, lengths and file offsets;
 * odd file extents use buffered io_uring on the same pinned inode, with no
 * read/modify/write of neighboring bytes. GDS stages on the GPU; native-only
 * still requires device RANGE elements and 4 KiB-aligned offset/total length.
 * No silent transport fallback. The original retain/release and drain rules
 * apply, including all-padding vectors. Limits: 16384 elements / 64 MiB.
 * transferred_bytes is the completed file prefix, INCLUDING padding; a short
 * READ scatters only that prefix. Copies/cleanup without a quiescence receipt
 * retain backing, staging and its budget; never replay a WRITE. */
int tent_file_submitv(tent_engine_t engine, tent_file_id_t file, int opcode,
                      uint64_t file_offset, const tent_file_element_t* elements,
                      size_t count, const tent_file_owner_t* owner,
                      tent_file_batch_t* batch);

/* cancel != 0 closes dispatch of subsequent windows; the current window may
 * finish. A timeout also closes dispatch. Neither is a physical receipt.
 * Returns UNQUIESCED with drained=0 on timeout or uncertain cleanup; keep
 * the engine and batch and retry drain (never resubmit a WRITE).
 * On drained=1 returns outcome (OK, SHORT_IO, IO_ERROR, CANCELED or TIMEOUT).
 * timeout_ms=0 polls once, without imposing a new operation deadline. */
int tent_file_drain(tent_engine_t engine, tent_file_batch_t batch,
                    uint64_t timeout_ms, int cancel,
                    tent_file_result_t* result);
int tent_file_release_batch(tent_engine_t engine, tent_file_batch_t batch);

/* Closes admission, cancels future windows and drains all admitted batches.
 * On UNQUIESCED the file/engine remain owned and close can be called again.
 * A successful close releases the inode; batch receipts still need release.
 * tent_destroy_engine retains an engine with any file handles or receipts. */
int tent_file_close(tent_engine_t engine, tent_file_id_t file,
                    uint64_t timeout_ms, tent_file_result_t* result);

struct tent_buffer_info {
    uint64_t base, length;
    char location[64];
};

#define TYPE_MEMORY (0)
#define TYPE_FILE (1)

struct tent_segment_info {
    int type;
    int num_buffers;
    struct tent_buffer_info* buffers;
};

typedef struct tent_segment_info tent_segment_info_t;

struct tent_notifi_record {
    tent_segment_id_t handle;
    char name[256];
    char msg[4096];
};

typedef struct tent_notifi_record tent_notifi_record_t;

struct tent_notifi_info {
    int num_records;
    struct tent_notifi_record* records;
};

typedef struct tent_notifi_info tent_notifi_info;

#define PERM_LOCAL_READ_WRITE (0)
#define PERM_GLOBAL_READ_ONLY (1)
#define PERM_GLOBAL_READ_WRITE (2)

#define TRANSPORT_UNSPEC (0)
#define TRANSPORT_RDMA (1)
#define TRANSPORT_MNNVL (2)
#define TRANSPORT_SHM (3)
#define TRANSPORT_NVLINK (4)
#define TRANSPORT_GDS (5)
#define TRANSPORT_IOURING (6)
#define TRANSPORT_TCP (7)
#define TRANSPORT_ASCEND_DIRECT (8)
#define TRANSPORT_SUNRISE_LINK (9)
#define TRANSPORT_TPU (10)
#define TRANSPORT_UB (11)
#define TRANSPORT_MPCOMM (12)
#define TRANSPORT_HP_TCP (13)
#define TRANSPORT_XPU (14)
#define TRANSPORT_HYLINK (15)
#define TRANSPORT_FABRIC (16)
#define TRANSPORT_MLU (17)
#define TRANSPORT_MEMFABRIC (18)

struct tent_memory_options {
    char location[64];
    int permission;     /* PERM_LOCAL_READ_WRITE, etc. */
    int transport_type; /* TRANSPORT_RDMA, etc. */
    char shm_path[256];
    size_t shm_offset;
    int internal; /* 0 = false, nonzero = true */
};

typedef struct tent_memory_options tent_memory_options_t;

void tent_load_config_from_file(const char* path);

void tent_set_config(const char* key, const char* value);

tent_engine_t tent_create_engine();

void tent_destroy_engine(tent_engine_t engine);
// Irreversibly close source admission, then await admitted CPU/TCP handlers.
// 0: drained; 1: still owned; 2: unsupported physical path; -1: error.
// epoch points to 33 bytes; active is observed after the bounded wait.
// This does not drain requester batches or authorize individual ticket reuse.
int tent_close_source(tent_engine_t engine, uint64_t timeout_ms, char* epoch,
                      uint64_t* active);

int tent_source_epoch(tent_engine_t engine, char* epoch, size_t capacity);
int tent_validate_remote_epoch(tent_engine_t engine, tent_segment_id_t segment,
                               const char* expected);

int tent_segment_name(tent_engine_t engine, char* buf, size_t buf_len);

int tent_rpc_server_addr_port(tent_engine_t engine, char* addr_buf,
                              size_t buf_len, uint16_t* port);

int tent_open_segment(tent_engine_t engine, tent_segment_id_t* handle,
                      const char* segment_name);

int tent_close_segment(tent_engine_t engine, tent_segment_id_t handle);

int tent_get_segment_info(tent_engine_t engine, tent_segment_id_t handle,
                          tent_segment_info_t* info);

void tent_free_segment_info(tent_segment_info_t* info);

int tent_allocate_memory(tent_engine_t engine, void** addr, size_t size,
                         const char* location);

int tent_free_memory(tent_engine_t engine, void* addr);

int tent_register_memory(tent_engine_t engine, void* addr, size_t size);

int tent_unregister_memory(tent_engine_t engine, void* addr, size_t size);

tent_batch_id_t tent_allocate_batch(tent_engine_t engine, size_t batch_size);

int tent_free_batch(tent_engine_t engine, tent_batch_id_t batch_id);

int tent_submit(tent_engine_t engine, tent_batch_id_t batch_id,
                tent_request_t* entries, size_t count);

int tent_submit_notif(tent_engine_t engine, tent_batch_id_t batch_id,
                      tent_request_t* entries, size_t count, const char* name,
                      const char* message);

int tent_send_notifs(tent_engine_t engine, tent_segment_id_t handle,
                     const char* name, const char* message);

/* Free the previous result before reusing info. Successful empty polls return
 * zero records and a null pointer. */
int tent_recv_notifs(tent_engine_t engine, tent_notifi_info* info);

/* Release records and reset info so it can be reused or freed again. */
void tent_free_notifs(tent_notifi_info* info);

int tent_task_status(tent_engine_t engine, tent_batch_id_t batch_id,
                     size_t task_id, tent_status_t* status);

int tent_cancel_task(tent_engine_t engine, tent_batch_id_t batch_id,
                     size_t task_id);

int tent_overall_status(tent_engine_t engine, tent_batch_id_t batch_id,
                        tent_status_t* status);

int tent_available(tent_engine_t engine);

int tent_register_memory_with_perm(tent_engine_t engine, void* addr,
                                   size_t size, int permission);

int tent_register_memory_batch(tent_engine_t engine, void** addrs,
                               size_t* sizes, size_t count, int permission);

int tent_unregister_memory_batch(tent_engine_t engine, void** addrs,
                                 size_t* sizes, size_t count);

int tent_allocate_memory_ex(tent_engine_t engine, void** addr, size_t size,
                            tent_memory_options_t* opts);

int tent_register_memory_ex(tent_engine_t engine, void* addr, size_t size,
                            tent_memory_options_t* opts);

int tent_register_memory_batch_ex(tent_engine_t engine, void** addrs,
                                  size_t* sizes, size_t count,
                                  tent_memory_options_t* opts);

int tent_task_status_list(tent_engine_t engine, tent_batch_id_t batch_id,
                          tent_status_t* statuses, size_t* count);

// Only NICs currently able to carry traffic are reported (see NicLoadStats).
struct tent_nic_load_stat {
    char device_name[64];
    uint64_t inflight_bytes;
    double ewma_bandwidth_bps;
};

typedef struct tent_nic_load_stat tent_nic_load_stat_t;

int tent_get_nic_load_stats(tent_engine_t engine, tent_nic_load_stat_t* stats,
                            size_t* count);

#ifdef __cplusplus
}
#endif  // __cplusplus

#ifdef __cplusplus
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tent/common/status.h"
#include "tent/common/types.h"

namespace mooncake {
class TransferEngine;
class TransferEngineImplTestPeer;
namespace tent {
class TransferEngineImpl;
class Config;
class Topology;
class TransferEngine {
   public:
    TransferEngine();

    TransferEngine(const std::string config_path);

    TransferEngine(std::shared_ptr<Config> config);

    ~TransferEngine();

    TransferEngine(const TransferEngine&) = delete;

    TransferEngine& operator=(const TransferEngine&) = delete;

   public:
    bool available() const;

    const std::string getSegmentName() const;

    const std::string getRpcServerAddress() const;

    uint16_t getRpcServerPort() const;

    // Returns the live local topology (nics/mems). Empty if engine unavailable.
    std::shared_ptr<Topology> getLocalTopology() const;

    // Native {"nics","mems"} JSON dump (includes rank0/1/2). "{}" if empty.
    std::string getLocalTopologyString() const;

   public:
    Status exportLocalSegment(std::string& shared_handle);

    Status importRemoteSegment(SegmentID& handle,
                               const std::string& shared_handle);

    Status openSegment(SegmentID& handle, const std::string& segment_name);

    Status closeSegment(SegmentID handle);

    Status getSegmentInfo(SegmentID handle, SegmentInfo& info);

   public:
    Status allocateLocalMemory(void** addr, size_t size,
                               Location location = kWildcardLocation);

    Status freeLocalMemory(void* addr);

    Status registerLocalMemory(void* addr, size_t size,
                               Permission permission = kGlobalReadWrite);

    Status unregisterLocalMemory(void* addr, size_t size = 0);

    Status registerLocalMemory(std::vector<void*> addr_list,
                               std::vector<size_t> size_list,
                               Permission permission = kGlobalReadWrite);

    Status unregisterLocalMemory(std::vector<void*> addr_list,
                                 std::vector<size_t> size_list = {});

    // advanced buffer allocate function
    Status allocateLocalMemory(void** addr, size_t size,
                               MemoryOptions& options);

    // advanced buffer register function
    Status registerLocalMemory(void* addr, size_t size, MemoryOptions& options);

    Status registerLocalMemory(std::vector<void*> addr_list,
                               std::vector<size_t> size_list,
                               MemoryOptions& options);

   public:
    BatchID allocateBatch(size_t batch_size);

    Status freeBatch(BatchID batch_id);

    Status submitTransfer(BatchID batch_id,
                          const std::vector<Request>& request_list);

    Status submitTransfer(BatchID batch_id,
                          const std::vector<Request>& request_list,
                          const Notification& notifi);

    // Best-effort task cancellation. Work that has not reached the transport
    // is prevented from being submitted. Device work already posted may still
    // complete, so callers must continue polling for a terminal status.
    Status cancelTransfer(BatchID batch_id, size_t task_id);

    Status sendNotification(SegmentID target_id, const Notification& notifi);

    Status receiveNotification(std::vector<Notification>& notifi_list);

    Status probePeerAliveByID(SegmentID target_id);

    Status getTransferStatus(BatchID batch_id, size_t task_id,
                             TransferStatus& status);

    Status getTransferStatus(BatchID batch_id,
                             std::vector<TransferStatus>& status_list);

    Status getTransferStatus(BatchID batch_id, TransferStatus& overall_status);

    // Drive one progress step on a batch and return its aggregated status.
    // Unlike getTransferStatus, this always allows internal failover/resubmit
    // regardless of enable_auto_failover_on_poll. The call is non-blocking and
    // performs at most one state-machine step per task; callers that want to
    // wait for completion must invoke it in a loop. PENDING means "make
    // progress later"; terminal states (COMPLETED/FAILED) will not be revived.
    Status progressBatch(BatchID batch_id, TransferStatus& overall_status);

    Status getNicLoadStats(std::vector<NicLoadStats>& stats) const;

   private:
    Status submitTransferRequiringPostSubmitCancellation(
        BatchID batch_id, const std::vector<Request>& request_list);

    std::unique_ptr<TransferEngineImpl> impl_;
    friend class ::mooncake::TransferEngine;
    friend class ::mooncake::TransferEngineImplTestPeer;
};
}  // namespace tent
}  // namespace mooncake
#endif

#endif
