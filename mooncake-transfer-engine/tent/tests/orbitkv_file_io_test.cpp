// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

#include "tent/runtime/file_io.h"
#include "tent/platform/cpu.h"
#include "tent/runtime/transfer_engine_impl.h"
#include "tent/transfer_engine.h"
#include "tent/transport/io_uring/io_uring_transport.h"

using namespace mooncake::tent;
using namespace std::chrono_literals;
extern "C" int file_c_api_read(tent_engine_t, tent_file_id_t,
                               tent_file_range_t*, size_t,
                               const tent_file_owner_t*, tent_file_result_t*);
extern "C" int file_c_api_vector(tent_engine_t, tent_file_id_t, int, uint64_t,
                                 const tent_file_element_t*, size_t,
                                 const tent_file_owner_t*, tent_file_result_t*);

namespace {
struct Counts {
    std::atomic<int> retained{0}, released{0}, destroyed{0};
};
struct Backing {
    void* bytes{nullptr};
    std::atomic<int> refs{1};
    std::shared_ptr<Counts> counts{std::make_shared<Counts>()};
    explicit Backing(size_t size) {
        if (posix_memalign(&bytes, 4096, size)) throw std::bad_alloc();
        memset(bytes, 0x7a, size);
    }
    ~Backing() {
        free(bytes);
        ++counts->destroyed;
    }
    static void retain(void* p) {
        auto* b = static_cast<Backing*>(p);
        ++b->refs;
        ++b->counts->retained;
    }
    static void release(void* p) {
        auto* b = static_cast<Backing*>(p);
        ++b->counts->released;
        drop(b);
    }
    static void drop(Backing* b) {
        if (--b->refs == 0) delete b;
    }
    tent_file_owner_t token() { return {this, retain, release}; }
};
using Buffer = std::unique_ptr<Backing, decltype(&Backing::drop)>;
Buffer buffer(size_t size) { return Buffer(new Backing(size), Backing::drop); }

// CPU shim at the production Platform::copy boundary. Only the device copy
// is simulated: submission, scratch ownership and file bytes are real.
class DeviceCopyShim : public CpuPlatform {
   public:
    uintptr_t begin;
    size_t length;
    std::atomic<int> gathers{0}, scatters{0};
    std::atomic<bool> fail{false}, hold{false}, entered{false};
    DeviceCopyShim(void* address, size_t size)
        : CpuPlatform(std::make_shared<Config>()),
          begin(reinterpret_cast<uintptr_t>(address)),
          length(size) {}
    MemoryType getMemoryType(void* p) override {
        auto address = reinterpret_cast<uintptr_t>(p);
        return address >= begin && address - begin < length ? MTYPE_CUDA
                                                            : MTYPE_CPU;
    }
    Status copy(void* dst, void* src, size_t size) override {
        if (getMemoryType(src) == MTYPE_CUDA) ++gathers;
        if (getMemoryType(dst) == MTYPE_CUDA) ++scatters;
        entered = true;
        while (hold) std::this_thread::sleep_for(1ms);
        if (fail) return Status::InternalError("injected uncertain copy");
        memcpy(dst, src, size);
        return Status::OK();
    }
};

// All I/O goes through the installed io_uring transport. Only completion
// observation or the cleanup return is injected; no fake byte movement.
class CompletionFault : public Transport {
   public:
    std::shared_ptr<Transport> real;
    std::atomic<bool> hold{false}, fail_poll{false}, fail_free{false};
    std::atomic<bool> hold_close{false}, fail_close{false};
    std::atomic<int> closes{0};
    std::atomic<int> polls{0}, submits{0}, frees{0};
    bool record_file_modes{false};
    std::vector<int> submitted_file_flags;
    std::weak_ptr<File> opened;
    Status openFile(int fd, FileRef& file) override {
        auto s = real->openFile(fd, file);
        opened = file;
        return s;
    }
    Status allocateSubBatch(SubBatchRef& b, size_t n) override {
        return real->allocateSubBatch(b, n);
    }
    Status closeFile(FileRef& file) override {
        ++closes;
        while (hold_close) std::this_thread::sleep_for(1ms);
        if (fail_close)
            return Status::InternalError("injected file cleanup failure");
        return real->closeFile(file);
    }
    Status submitTransferTasks(SubBatchRef b,
                               const std::vector<Request>& r) override {
        ++submits;
        auto status = real->submitTransferTasks(b, r);
        if (record_file_modes && status.ok()) {
            // Each staged window owns a fresh ring with one SQE. Inspect the
            // actual submitted fd, not the range-alignment selection rule.
            // The kernel consumes SQEs without changing their contents.
            auto* native = dynamic_cast<IOUringSubBatch*>(b);
            if (!native || r.size() != 1)
                return Status::InternalError("Expected one staged file SQE");
            submitted_file_flags.push_back(
                fcntl(native->ring.sq.sqes[0].fd, F_GETFL));
        }
        return status;
    }
    Status getTransferStatus(SubBatchRef b, int i, TransferStatus& s) override {
        ++polls;
        if (fail_poll) return Status::InternalError("injected lost completion");
        if (hold) {
            s = {PENDING, 0};
            return Status::OK();
        }
        return real->getTransferStatus(b, i, s);
    }
    bool fileTaskShortIO(SubBatchRef b, int i) const override {
        return real->fileTaskShortIO(b, i);
    }
    Status freeSubBatch(SubBatchRef& b) override {
        ++frees;
        if (fail_free) return Status::InternalError("injected cleanup failure");
        return real->freeSubBatch(b);
    }
};

tent_file_options_t options() {
    return {sizeof(tent_file_options_t), 1, TENT_GDS_OFF, 1, 1};
}
void configure() {
    tent_set_config("metadata_type", "p2p");
    tent_set_config("metadata_servers", "");
    tent_set_config("rpc_server_hostname", "127.0.0.1");
    tent_set_config("rpc_server_port", "0");
    tent_set_config("rpc_server_threads", "1");
    tent_set_config("transports/tcp/enable", "true");
    for (const char* name :
         {"rdma", "shm", "nvlink", "mnnvl", "hp_tcp", "fabric"}) {
        const auto key = std::string("transports/") + name + "/enable";
        tent_set_config(key.c_str(), "false");
    }
    // Typed options must override the initializer's legacy disabled setting.
    tent_set_config("transports/io_uring/enable", "false");
    tent_set_config("transports/gds/enable", "false");
}

class FileCApi : public testing::Test {
   protected:
    tent_engine_t engine{nullptr};
    int fd{-1};
    std::string path, uri;
    std::vector<tent_file_id_t> files;
    std::vector<tent_file_batch_t> batches;
    std::shared_ptr<CompletionFault> fault;

    virtual const char* testDirectory() const {
        const char* directory = std::getenv("TENT_FILE_TEST_DIR");
        return directory ? directory : "/dev/shm";
    }

    void SetUp() override {
        configure();
        auto o = options();
        // This gate requires real io_uring. A blocked/missing kernel fails.
        ASSERT_EQ(tent_create_engine_with_files(&o, &engine), TENT_FILE_OK);
        ASSERT_NE(engine, nullptr);
        fault = std::make_shared<CompletionFault>();
        fault->real = static_cast<TransferEngineImpl*>(engine)
                          ->fileIO()
                          ->replaceUringForTest(fault);
        ASSERT_NE(fault->real, nullptr);
        const std::string pattern =
            std::string(testDirectory()) + "/tent-file-c-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        fd = mkstemp(name.data());
        ASSERT_GE(fd, 0);
        path = name.data();
        uri = "file://" + path;
        ASSERT_EQ(ftruncate(fd, 64 * 4096), 0);
    }
    void TearDown() override {
        if (fault) {
            fault->hold = false;
            fault->fail_poll = false;
            fault->hold_close = false;
        }
        if (engine) {
            for (auto b : batches) {
                auto result = finish(b);
                EXPECT_EQ(result.drained, 1u);
                EXPECT_EQ(tent_file_release_batch(engine, b), TENT_FILE_OK);
            }
            for (auto f : files) {
                tent_file_result_t result{};
                EXPECT_EQ(tent_file_close(engine, f, 5000, &result),
                          TENT_FILE_OK);
                EXPECT_EQ(result.drained, 1u);
            }
            tent_destroy_engine(engine);
        }
        if (fd >= 0) ::close(fd);
        if (!path.empty()) unlink(path.c_str());
    }
    tent_file_id_t open(tent_file_access_t access = TENT_FILE_READ_WRITE) {
        tent_file_id_t f = 0;
        uint64_t length = 0;
        EXPECT_EQ(tent_file_open(engine, uri.c_str(), access, TRANSPORT_IOURING,
                                 &f, &length),
                  TENT_FILE_OK);
        EXPECT_EQ(length, 64u * 4096);
        if (f) files.push_back(f);
        return f;
    }
    tent_file_batch_t submit(tent_file_id_t f, int opcode, uint64_t offset,
                             const std::vector<tent_file_range_t>& ranges,
                             Backing& backing) {
        tent_file_batch_t b = 0;
        auto owner = backing.token();
        EXPECT_EQ(tent_file_submit(engine, f, opcode, offset, ranges.data(),
                                   ranges.size(), &owner, &b),
                  TENT_FILE_OK);
        if (b) batches.push_back(b);
        return b;
    }
    tent_file_result_t finish(tent_file_batch_t b) {
        tent_file_result_t result{};
        const auto end = std::chrono::steady_clock::now() + 5s;
        do {
            tent_file_drain(engine, b, 0, 0, &result);
            if (result.drained) return result;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < end);
        return result;
    }
    FileIO* fileIO() {
        return static_cast<TransferEngineImpl*>(engine)->fileIO();
    }
    tent_file_batch_t submitv(tent_file_id_t f, int opcode, uint64_t offset,
                              const std::vector<tent_file_element_t>& elements,
                              Backing& backing) {
        tent_file_batch_t b = 0;
        auto owner = backing.token();
        EXPECT_EQ(tent_file_submitv(engine, f, opcode, offset, elements.data(),
                                    elements.size(), &owner, &b),
                  TENT_FILE_OK);
        if (b) batches.push_back(b);
        return b;
    }
    tent_file_id_t largeFile(size_t length) {
        EXPECT_EQ(ftruncate(fd, length), 0);
        tent_file_id_t f = 0;
        uint64_t actual = 0;
        EXPECT_EQ(tent_file_open(engine, uri.c_str(), TENT_FILE_READ_WRITE,
                                 TRANSPORT_IOURING, &f, &actual),
                  TENT_FILE_OK);
        EXPECT_EQ(actual, length);
        if (f) files.push_back(f);
        return f;
    }
    void awaitPoll() {
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (!fault->polls && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(1ms);
        ASSERT_GT(fault->polls, 0);
    }
};

// Kept in a separate CTest invocation: tmpfs/overlayfs cannot qualify this
// contract. A missing required facility is a failure, never a skip.
class FileDirectCoherence : public FileCApi {
   protected:
    const char* testDirectory() const override {
        const char* directory = std::getenv("TENT_FILE_DIRECT_DIR");
        return directory ? directory : "/tmp";
    }
    void SetUp() override {
        const char* required = std::getenv("TENT_FILE_REQUIRE_DIRECT");
        ASSERT_STREQ(required ? required : "", "1")
            << "Set TENT_FILE_REQUIRE_DIRECT=1 and TENT_FILE_DIRECT_DIR to "
               "an O_DIRECT-capable ext4/XFS directory";
        FileCApi::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        struct statfs fs{};
        ASSERT_EQ(fstatfs(fd, &fs), 0);
        ASSERT_TRUE(fs.f_type == EXT4_SUPER_MAGIC ||
                    fs.f_type == XFS_SUPER_MAGIC)
            << "Required coherence gate needs ext4/XFS, not tmpfs/overlayfs; "
            << "filesystem magic=" << std::hex << fs.f_type;
        const int direct = ::open(path.c_str(), O_RDWR | O_DIRECT | O_CLOEXEC);
        ASSERT_GE(direct, 0)
            << "Required O_DIRECT open failed: " << strerror(errno);
        const int flags = fcntl(direct, F_GETFL);
        EXPECT_EQ(::close(direct), 0);
        ASSERT_GE(flags, 0);
        ASSERT_NE(flags & O_DIRECT, 0);
        fault->submitted_file_flags.reserve(8);
        fault->record_file_modes = true;
    }
    void completed(tent_file_batch_t batch, uint64_t bytes) {
        const auto result = finish(batch);
        ASSERT_EQ(result.drained, 1u);
        ASSERT_EQ(result.outcome, TENT_FILE_OK);
        ASSERT_EQ(result.transferred_bytes, bytes);
        EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
    }
    void modes(std::initializer_list<bool> expected) {
        ASSERT_EQ(fault->submitted_file_flags.size(), expected.size());
        size_t i = 0;
        for (bool direct : expected) {
            const int flags = fault->submitted_file_flags[i++];
            ASSERT_GE(flags, 0);
            EXPECT_EQ((flags & O_DIRECT) != 0, direct) << "submission " << i;
        }
    }
};

TEST_F(FileDirectCoherence, OddBufferedWriteThenEnclosingDirectRead) {
    auto f = open();
    auto source = buffer(8192), destination = buffer(8192);
    auto* data = static_cast<unsigned char*>(source->bytes);
    memset(data, 0x31, 8192);
    ASSERT_NO_FATAL_FAILURE(
        completed(submitv(f, OPCODE_WRITE, 0,
                          {{TENT_FILE_ELEMENT_RANGE, data, 8192}}, *source),
                  8192));
    std::vector<unsigned char> expected(8192, 0x31);
    for (size_t i = 0; i < 1117; ++i)
        expected[37 + i] = data[1 + i] = (i * 17 + 7) % 251;
    ASSERT_NO_FATAL_FAILURE(
        completed(submitv(f, OPCODE_WRITE, 37,
                          {{TENT_FILE_ELEMENT_RANGE, data + 1, 1117}}, *source),
                  1117));
    // No fsync, fadvise or cache drop: the direct READ must flush dirty
    // buffered data before reading the enclosing aligned extent.
    ASSERT_NO_FATAL_FAILURE(
        completed(submitv(f, OPCODE_READ, 0,
                          {{TENT_FILE_ELEMENT_RANGE, destination->bytes, 8192}},
                          *destination),
                  8192));
    ASSERT_NO_FATAL_FAILURE(modes({true, false, true}));
    const auto* actual = static_cast<unsigned char*>(destination->bytes);
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(actual[i], expected[i]) << i;
}

TEST_F(FileDirectCoherence, DirectWriteInvalidatesWarmBufferedRead) {
    auto f = open();
    auto source = buffer(8192), destination = buffer(4096);
    auto* data = static_cast<unsigned char*>(source->bytes);
    auto* actual = static_cast<unsigned char*>(destination->bytes);
    memset(data, 0x43, 8192);
    ASSERT_NO_FATAL_FAILURE(
        completed(submitv(f, OPCODE_WRITE, 0,
                          {{TENT_FILE_ELEMENT_RANGE, data, 8192}}, *source),
                  8192));
    // Populate the buffered cache with OLD bytes before the direct overwrite.
    ASSERT_NO_FATAL_FAILURE(completed(
        submitv(f, OPCODE_READ, 37,
                {{TENT_FILE_ELEMENT_RANGE, actual + 1, 1117}}, *destination),
        1117));
    for (size_t i = 1; i <= 1117; ++i) ASSERT_EQ(actual[i], 0x43) << i;
    for (size_t i = 0; i < 8192; ++i) data[i] = (i * 29 + 11) % 251;
    ASSERT_NO_FATAL_FAILURE(
        completed(submitv(f, OPCODE_WRITE, 0,
                          {{TENT_FILE_ELEMENT_RANGE, data, 8192}}, *source),
                  8192));
    memset(actual, 0xac, 4096);
    ASSERT_NO_FATAL_FAILURE(completed(
        submitv(f, OPCODE_READ, 37,
                {{TENT_FILE_ELEMENT_RANGE, actual + 1, 1117}}, *destination),
        1117));
    ASSERT_NO_FATAL_FAILURE(modes({true, false, true, false}));
    for (size_t i = 0; i < 4096; ++i)
        ASSERT_EQ(actual[i], i >= 1 && i <= 1117 ? data[36 + i] : 0xac) << i;
}

TEST_F(FileDirectCoherence, StagedWindowsSwitchDirectToBufferedThenReadBack) {
    constexpr size_t window = 4 << 20, enclosing = window + 4096;
    auto f = largeFile(enclosing);
    auto source = buffer(window + 8192), destination = buffer(enclosing);
    auto* data = static_cast<unsigned char*>(source->bytes);
    memset(data, 0x95, enclosing);
    ASSERT_NO_FATAL_FAILURE(completed(
        submitv(f, OPCODE_WRITE, 0,
                {{TENT_FILE_ELEMENT_RANGE, data, enclosing}}, *source),
        enclosing));
    fault->submitted_file_flags.clear();
    std::vector<unsigned char> expected(enclosing, 0x95);
    for (size_t i = 0; i < window - 5; ++i)
        expected[i] = data[1 + i] = (i * 13 + 5) % 251;
    std::fill(expected.begin() + window - 5, expected.begin() + window + 37, 0);
    for (size_t i = 0; i < 23; ++i)
        expected[window + 4 + i] = data[window + 32 + i] = 0xe0 + i;
    // A 4 MiB direct window followed by a 37-byte buffered window. Padding
    // crosses the window boundary; the enclosing tail must remain unchanged.
    ASSERT_NO_FATAL_FAILURE(
        completed(submitv(f, OPCODE_WRITE, 0,
                          {{TENT_FILE_ELEMENT_RANGE, data + 1, window - 5},
                           {TENT_FILE_ELEMENT_PADDING, nullptr, 9},
                           {TENT_FILE_ELEMENT_RANGE, data + window + 32, 23},
                           {TENT_FILE_ELEMENT_PADDING, nullptr, 10}},
                          *source),
                  window + 37));
    ASSERT_NO_FATAL_FAILURE(modes({true, false}));
    ASSERT_NO_FATAL_FAILURE(completed(
        submitv(f, OPCODE_READ, 0,
                {{TENT_FILE_ELEMENT_RANGE, destination->bytes, enclosing}},
                *destination),
        enclosing));
    ASSERT_NO_FATAL_FAILURE(modes({true, false, true, true}));
    const auto* actual = static_cast<unsigned char*>(destination->bytes);
    for (size_t i = 0; i < expected.size(); ++i)
        ASSERT_EQ(actual[i], expected[i]) << i;
}

TEST_F(FileCApi, VectorsRoundTripThroughCTranslationUnit) {
    auto f = open();
    auto b = buffer(3 * 4096);
    std::vector<tent_file_range_t> ranges;
    for (int i = 0; i < 3; ++i) {
        auto* address = static_cast<char*>(b->bytes) + i * 4096;
        memset(address, 0x21 + i, 4096);
        ranges.push_back({address, 4096});
    }
    auto write = submit(f, OPCODE_WRITE, 4096, ranges, *b);
    auto result = finish(write);
    ASSERT_EQ(result.drained, 1u);
    ASSERT_EQ(result.outcome, TENT_FILE_OK);
    ASSERT_EQ(result.transferred_bytes, 3u * 4096);
    memset(b->bytes, 0, 3 * 4096);
    auto owner = b->token();
    ASSERT_EQ(file_c_api_read(engine, f, ranges.data(), ranges.size(), &owner,
                              &result),
              TENT_FILE_OK);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.transferred_bytes, 3u * 4096);
    for (int i = 0; i < 3 * 4096; ++i)
        ASSERT_EQ(static_cast<unsigned char*>(b->bytes)[i], 0x21 + i / 4096);
    EXPECT_EQ(b->counts->retained, 2);
    EXPECT_EQ(b->counts->released, 2);
    EXPECT_EQ(fault->submits, 6);  // queue depth one, no replay
}

TEST_F(FileCApi, PermissionsAndOverflowRejectBeforeOwnerTransfer) {
    auto ro = open(TENT_FILE_READ), wo = open(TENT_FILE_WRITE);
    auto b = buffer(4096);
    auto owner = b->token();
    tent_file_range_t range{b->bytes, 4096};
    tent_file_batch_t id = 99;
    EXPECT_EQ(
        tent_file_submit(engine, ro, OPCODE_WRITE, 0, &range, 1, &owner, &id),
        TENT_FILE_PERMISSION);
    EXPECT_EQ(id, 0u);
    EXPECT_EQ(
        tent_file_submit(engine, wo, OPCODE_READ, 0, &range, 1, &owner, &id),
        TENT_FILE_PERMISSION);
    EXPECT_EQ(tent_file_submit(engine, wo, OPCODE_WRITE, UINT64_MAX, &range, 1,
                               &owner, &id),
              TENT_FILE_INVALID_ARGUMENT);
    range.length = UINT64_MAX;
    EXPECT_EQ(
        tent_file_submit(engine, wo, OPCODE_WRITE, 0, &range, 1, &owner, &id),
        TENT_FILE_INVALID_ARGUMENT);
    EXPECT_EQ(b->counts->retained, 0);
    EXPECT_EQ(fault->submits, 0);
}

TEST_F(FileCApi, TruncationShortReadCopiesOnlyCompletedPrefix) {
    auto f = open(TENT_FILE_READ);
    std::vector<char> contents(4096, 'Q');
    ASSERT_EQ(pwrite(fd, contents.data(), contents.size(), 0), 4096);
    ASSERT_EQ(ftruncate(fd, 4096), 0);
    auto b = buffer(8192 + 4096);
    auto* unaligned = static_cast<char*>(b->bytes) + 1;
    auto id = submit(f, OPCODE_READ, 0, {{unaligned, 8192}}, *b);
    auto result = finish(id);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_SHORT_IO);
    EXPECT_EQ(result.transferred_bytes, 4096u);
    for (int i = 0; i < 8192; ++i)
        ASSERT_EQ(unaligned[i], i < 4096 ? 'Q' : 0x7a);
    auto eof = submit(f, OPCODE_READ, 4096, {{unaligned, 4096}}, *b);
    result = finish(eof);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_SHORT_IO);
    EXPECT_EQ(result.transferred_bytes, 0u);
}

TEST_F(FileCApi, CancelDrainsCurrentWindowAndStopsTheTail) {
    auto f = open();
    auto b = buffer(8192);
    fault->hold = true;
    auto id = submit(
        f, OPCODE_WRITE, 0,
        {{b->bytes, 4096}, {static_cast<char*>(b->bytes) + 4096, 4096}}, *b);
    awaitPoll();
    tent_file_result_t result{};
    ASSERT_EQ(tent_file_drain(engine, id, 0, 1, &result), TENT_FILE_UNQUIESCED);
    ASSERT_EQ(result.drained, 0u);
    EXPECT_EQ(b->counts->released, 0);
    fault->hold = false;
    result = finish(id);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_CANCELED);
    EXPECT_EQ(result.transferred_bytes, 4096u);
    EXPECT_EQ(fault->submits, 1);
    char tail[4096];
    ASSERT_EQ(pread(fd, tail, sizeof(tail), 4096), sizeof(tail));
    for (char c : tail) ASSERT_EQ(c, 0);
}

TEST_F(FileCApi, DeadlineIsNotAReleaseReceipt) {
    auto f = open();
    auto b = buffer(4096);
    fault->hold = true;
    auto id = submit(f, OPCODE_READ, 0, {{b->bytes, 4096}}, *b);
    awaitPoll();
    tent_file_result_t result{};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(tent_file_drain(engine, id, 5, 0, &result), TENT_FILE_UNQUIESCED);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(result.outcome, TENT_FILE_TIMEOUT);
    EXPECT_EQ(result.drained, 0u);
    EXPECT_EQ(b->counts->released, 0);
    fault->hold = false;
    result = finish(id);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_TIMEOUT);
}

TEST_F(FileCApi, LostCompletionRetainsActualBackingAndEngineThroughClose) {
    auto f = open();
    auto b = buffer(4096);
    auto counts = b->counts;
    fault->fail_poll = true;
    auto id = submit(f, OPCODE_WRITE, 0, {{b->bytes, 4096}}, *b);
    b.reset();  // only the C batch now owns the actual allocation
    awaitPoll();
    tent_file_result_t result{};
    EXPECT_EQ(tent_file_drain(engine, id, 5, 0, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(result.drained, 0u);
    EXPECT_EQ(tent_file_close(engine, f, 5, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(tent_file_release_batch(engine, id), TENT_FILE_UNQUIESCED);
    tent_destroy_engine(engine);  // must retain, not invalidate the pointer
    EXPECT_EQ(tent_available(engine), 1);
    EXPECT_EQ(counts->released, 0);
    EXPECT_EQ(counts->destroyed, 0);
    EXPECT_FALSE(fault->opened.expired());
    fault->fail_poll = false;
    result = finish(id);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_IO_ERROR);
    EXPECT_EQ(counts->released, 1);
    EXPECT_EQ(counts->destroyed, 1);
    EXPECT_EQ(fault->submits, 1);
    EXPECT_EQ(tent_file_close(engine, f, 5000, &result), TENT_FILE_OK);
    files.clear();
    EXPECT_TRUE(fault->opened.expired());
}

TEST_F(FileCApi, RetainedBatchesRemainChargedToTheBound) {
    auto f = open();
    auto b = buffer(4096);
    fault->hold = true;
    for (size_t i = 0; i < 32; ++i)
        submit(f, OPCODE_WRITE, i * 4096, {{b->bytes, 4096}}, *b);
    tent_file_range_t range{b->bytes, 4096};
    auto owner = b->token();
    tent_file_batch_t extra = 0;
    EXPECT_EQ(
        tent_file_submit(engine, f, OPCODE_WRITE, 0, &range, 1, &owner, &extra),
        TENT_FILE_BUSY);
    EXPECT_EQ(extra, 0u);
    EXPECT_EQ(b->counts->retained, 32);
    EXPECT_EQ(b->counts->released, 0);
    tent_file_result_t result{};
    EXPECT_EQ(tent_file_close(engine, f, 0, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(result.drained, 0u);
    fault->hold = false;
    for (auto id : batches) ASSERT_EQ(finish(id).drained, 1u);
    EXPECT_EQ(b->counts->released, 32);
}

TEST_F(FileCApi, OpenPinsTheInodeAndCloseReleasesIt) {
    auto f = open();
    ASSERT_EQ(unlink(path.c_str()), 0);
    int replacement = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(replacement, 0);
    ASSERT_EQ(ftruncate(replacement, 4096), 0);
    auto b = buffer(4096);
    auto id = submit(f, OPCODE_WRITE, 0, {{b->bytes, 4096}}, *b);
    ASSERT_EQ(finish(id).outcome, TENT_FILE_OK);
    char old_bytes[4096], new_bytes[4096];
    ASSERT_EQ(pread(fd, old_bytes, sizeof(old_bytes), 0), sizeof(old_bytes));
    ASSERT_EQ(pread(replacement, new_bytes, sizeof(new_bytes), 0),
              sizeof(new_bytes));
    ::close(replacement);
    for (size_t i = 0; i < sizeof(old_bytes); ++i) {
        ASSERT_EQ(old_bytes[i], 0x7a);
        ASSERT_EQ(new_bytes[i], 0);
    }
    tent_file_result_t result{};
    ASSERT_EQ(tent_file_close(engine, f, 5000, &result), TENT_FILE_OK);
    files.clear();
    EXPECT_EQ(result.drained, 1u);
    EXPECT_TRUE(fault->opened.expired());
}

TEST_F(FileCApi, GdsDisabledIsDistinctFromUnavailable) {
    tent_file_id_t f = 0;
    uint64_t length = 0;
    EXPECT_EQ(tent_file_open(engine, uri.c_str(), TENT_FILE_READ, TRANSPORT_GDS,
                             &f, &length),
              TENT_FILE_DISABLED);
    // CPU gate: either cuFile was not built or the runner has no driver.
    auto o = options();
    o.gds_mode = TENT_GDS_ON;
    tent_engine_t absent = nullptr;
    EXPECT_EQ(tent_create_engine_with_files(&o, &absent),
              TENT_FILE_UNAVAILABLE);
    EXPECT_EQ(absent, nullptr);
    o.gds_mode = TENT_GDS_NATIVE_ONLY;
    EXPECT_EQ(tent_create_engine_with_files(&o, &absent),
              TENT_FILE_UNAVAILABLE);
    EXPECT_EQ(absent, nullptr);
    o = options();
    o.io_uring_queue_depth = 0;
    EXPECT_EQ(tent_create_engine_with_files(&o, &absent),
              TENT_FILE_INVALID_ARGUMENT);
}

TEST_F(FileCApi, DisabledFileTransportsRemainExplicitlyDisabled) {
    auto o = options();
    o.io_uring_enabled = 0;
    o.io_uring_queue_depth = 0;
    tent_engine_t disabled = nullptr;
    ASSERT_EQ(tent_create_engine_with_files(&o, &disabled), TENT_FILE_OK);
    tent_file_id_t f = 0;
    uint64_t length = 0;
    EXPECT_EQ(tent_file_open(disabled, uri.c_str(), TENT_FILE_READ,
                             TRANSPORT_IOURING, &f, &length),
              TENT_FILE_DISABLED);
    EXPECT_EQ(f, 0u);
    tent_destroy_engine(disabled);
    auto legacy = tent_create_engine();
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(tent_file_open(legacy, uri.c_str(), TENT_FILE_READ,
                             TRANSPORT_IOURING, &f, &length),
              TENT_FILE_DISABLED);
    tent_destroy_engine(legacy);
}

TEST_F(FileCApi, FileEnablementDoesNotWeakenSourceDrain) {
    char epoch[33]{};
    uint64_t active = 99;
    EXPECT_EQ(tent_close_source(engine, 5, epoch, &active), 2);
}

TEST_F(FileCApi, CleanupFailureIsRetainedAndNeverRetried) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ASSERT_EXIT(
        {
            auto f = open();
            auto b = buffer(4096);
            fault->fail_free = true;
            auto id = submit(f, OPCODE_WRITE, 0, {{b->bytes, 4096}}, *b);
            const auto end = std::chrono::steady_clock::now() + 5s;
            while (!fault->frees && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(1ms);
            tent_file_result_t result{};
            for (int i = 0; i < 3; ++i) {
                if (tent_file_drain(engine, id, 5, 1, &result) !=
                        TENT_FILE_UNQUIESCED ||
                    result.drained ||
                    tent_file_release_batch(engine, id) != TENT_FILE_UNQUIESCED)
                    _exit(1);
            }
            tent_destroy_engine(engine);
            if (fault->frees != 1 || fault->submits != 1 ||
                b->counts->released != 0 || fault->opened.expired())
                _exit(2);
            _exit(0);  // intentional retained cleanup owner lives until process
                       // exit
        },
        testing::ExitedWithCode(0), "");
}

TEST_F(FileCApi, FileCloseDeadlineRetainsNativeHandle) {
    auto f = open();
    fault->hold_close = true;
    tent_file_result_t result{};
    EXPECT_EQ(tent_file_close(engine, f, 5, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(result.drained, 0u);
    EXPECT_FALSE(fault->opened.expired());
    fault->hold_close = false;
    EXPECT_EQ(tent_file_close(engine, f, 5000, &result), TENT_FILE_OK);
    files.clear();
    EXPECT_EQ(result.drained, 1u);
    EXPECT_EQ(fault->closes, 1);
    EXPECT_TRUE(fault->opened.expired());
}

TEST_F(FileCApi, FileCleanupFailureDoesNotRetryNativeDestroy) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ASSERT_EXIT(
        {
            auto f = open();
            fault->fail_close = true;
            tent_file_result_t result{};
            for (int i = 0; i < 3; ++i) {
                if (tent_file_close(engine, f, 5000, &result) !=
                        TENT_FILE_UNQUIESCED ||
                    result.drained)
                    _exit(1);
            }
            tent_destroy_engine(engine);
            if (fault->closes != 1 || fault->opened.expired()) _exit(2);
            _exit(0);
        },
        testing::ExitedWithCode(0), "");
}
TEST_F(FileCApi, PaddingWritesZerosAndReadsDiscardThroughCTranslationUnit) {
    auto f = open();
    auto b = buffer(4096);
    auto* data = static_cast<unsigned char*>(b->bytes);
    std::vector<unsigned char> disk(4096, 0xa5);
    ASSERT_EQ(pwrite(fd, disk.data(), disk.size(), 0), 4096);
    memset(data + 1, 0x31, 3);
    memset(data + 19, 0x72, 7);
    const tent_file_element_t elements[] = {
        {TENT_FILE_ELEMENT_RANGE, data + 1, 3},
        {TENT_FILE_ELEMENT_PADDING, nullptr, 5},
        {TENT_FILE_ELEMENT_RANGE, data + 19, 7},
        {TENT_FILE_ELEMENT_PADDING, nullptr, 11}};
    auto owner = b->token();
    tent_file_result_t result{};
    ASSERT_EQ(file_c_api_vector(engine, f, OPCODE_WRITE, 13, elements, 4,
                                &owner, &result),
              TENT_FILE_OK);
    EXPECT_EQ(result.drained, 1u);
    EXPECT_EQ(result.transferred_bytes, 26u);
    ASSERT_EQ(pread(fd, disk.data(), disk.size(), 0), 4096);
    for (size_t i = 0; i < disk.size(); ++i) {
        unsigned char expected = 0xa5;
        if (i >= 13 && i < 39) expected = 0;
        if (i >= 13 && i < 16) expected = 0x31;
        if (i >= 21 && i < 28) expected = 0x72;
        ASSERT_EQ(disk[i], expected) << i;
    }
    // Padding consumes nonzero file data but has no caller destination.
    memset(disk.data() + 16, 0xe3, 5);
    memset(disk.data() + 28, 0xe4, 11);
    ASSERT_EQ(pwrite(fd, disk.data(), disk.size(), 0), 4096);
    memset(data, 0xcc, 4096);
    ASSERT_EQ(file_c_api_vector(engine, f, OPCODE_READ, 13, elements, 4, &owner,
                                &result),
              TENT_FILE_OK);
    EXPECT_EQ(result.transferred_bytes, 26u);
    for (size_t i = 0; i < 4096; ++i) {
        unsigned char expected = 0xcc;
        if (i >= 1 && i < 4) expected = 0x31;
        if (i >= 19 && i < 26) expected = 0x72;
        ASSERT_EQ(data[i], expected) << i;
    }
    EXPECT_EQ(fault->submits, 2);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
}

TEST_F(FileCApi, LegacyUnalignedVectorsUseOwnedStaging) {
    auto f = open();
    auto b = buffer(4096);
    auto* data = static_cast<unsigned char*>(b->bytes);
    memset(data + 1, 0x37, 7);
    memset(data + 23, 0x61, 17);
    const std::vector<tent_file_range_t> ranges{{data + 1, 7}, {data + 23, 17}};
    auto write = submit(f, OPCODE_WRITE, 19, ranges, *b);
    auto result = finish(write);
    ASSERT_EQ(result.outcome, TENT_FILE_OK);
    ASSERT_EQ(result.transferred_bytes, 24u);
    memset(data, 0xa9, 4096);
    auto read = submit(f, OPCODE_READ, 19, ranges, *b);
    result = finish(read);
    ASSERT_EQ(result.outcome, TENT_FILE_OK);
    ASSERT_EQ(result.transferred_bytes, 24u);
    for (size_t i = 0; i < 4096; ++i) {
        unsigned char expected = 0xa9;
        if (i >= 1 && i < 8) expected = 0x37;
        if (i >= 23 && i < 40) expected = 0x61;
        ASSERT_EQ(data[i], expected) << i;
    }
    EXPECT_EQ(fault->submits, 2);
}

TEST_F(FileCApi, ShortReadInsidePaddingReportsFilePrefixAndLeavesTail) {
    auto f = open();
    auto b = buffer(4096);
    auto* data = static_cast<unsigned char*>(b->bytes);
    const unsigned char disk[]{1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(pwrite(fd, disk, sizeof(disk), 3), 7);
    ASSERT_EQ(ftruncate(fd, 10), 0);  // descriptor retains its admitted extent
    auto read = submitv(f, OPCODE_READ, 3,
                        {{TENT_FILE_ELEMENT_RANGE, data + 1, 3},
                         {TENT_FILE_ELEMENT_PADDING, nullptr, 10},
                         {TENT_FILE_ELEMENT_RANGE, data + 31, 5}},
                        *b);
    auto result = finish(read);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_SHORT_IO);
    EXPECT_EQ(result.transferred_bytes, 7u);  // 3 data + 4 discarded padding
    for (size_t i = 0; i < 4096; ++i)
        ASSERT_EQ(data[i], i >= 1 && i < 4 ? i : 0x7a) << i;
    EXPECT_EQ(fault->submits, 1);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
}

TEST_F(FileCApi, PaddingAndRangesCrossStagingWindows) {
    constexpr size_t window = 4 << 20;
    auto f = largeFile(window + 8192);
    auto b = buffer(8192);
    auto* data = static_cast<unsigned char*>(b->bytes);
    memset(data + 1, 0x31, 7);
    memset(data + 31, 0x62, 73);
    const std::vector<tent_file_element_t> elements{
        {TENT_FILE_ELEMENT_RANGE, data + 1, 7},
        {TENT_FILE_ELEMENT_PADDING, nullptr, window + 5},
        {TENT_FILE_ELEMENT_RANGE, data + 31, 73},
        {TENT_FILE_ELEMENT_PADDING, nullptr, 11}};
    auto write = submitv(f, OPCODE_WRITE, 1, elements, *b);
    auto result = finish(write);
    ASSERT_EQ(result.outcome, TENT_FILE_OK);
    EXPECT_EQ(result.transferred_bytes, window + 96u);
    std::vector<unsigned char> disk(window + 97);
    ASSERT_EQ(pread(fd, disk.data(), disk.size(), 0), disk.size());
    for (size_t i = 0; i < disk.size(); ++i) {
        unsigned char expected = 0;
        if (i >= 1 && i < 8) expected = 0x31;
        if (i >= window + 13 && i < window + 86) expected = 0x62;
        ASSERT_EQ(disk[i], expected) << i;
    }
    memset(data, 0xab, 8192);
    auto read = submitv(f, OPCODE_READ, 1, elements, *b);
    result = finish(read);
    ASSERT_EQ(result.outcome, TENT_FILE_OK);
    EXPECT_EQ(result.transferred_bytes, window + 96u);
    for (size_t i = 0; i < 8192; ++i) {
        unsigned char expected = 0xab;
        if (i >= 1 && i < 8) expected = 0x31;
        if (i >= 31 && i < 104) expected = 0x62;
        ASSERT_EQ(data[i], expected) << i;
    }
    EXPECT_EQ(fault->submits, 4);
}

TEST_F(FileCApi, StagedCancelRetainsScratchAndStopsNextWindow) {
    constexpr size_t window = 4 << 20;
    auto f = largeFile(window + 4096);
    auto b = buffer(1);
    fault->hold = true;
    auto id = submitv(f, OPCODE_WRITE, 0,
                      {{TENT_FILE_ELEMENT_PADDING, nullptr, window + 1}}, *b);
    awaitPoll();
    tent_file_result_t result{};
    EXPECT_EQ(tent_file_drain(engine, id, 0, 1, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(result.drained, 0u);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), window);
    EXPECT_EQ(b->counts->released, 0);
    fault->hold = false;
    result = finish(id);
    ASSERT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_CANCELED);
    EXPECT_EQ(result.transferred_bytes, window);
    EXPECT_EQ(fault->submits, 1);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
}

TEST_F(FileCApi, StagedLostCompletionRetainsOwnersAndBudgetWithoutReplay) {
    auto f = open();
    auto b = buffer(4096);
    auto counts = b->counts;
    fault->fail_poll = true;
    auto id = submitv(f, OPCODE_WRITE, 5,
                      {{TENT_FILE_ELEMENT_RANGE, b->bytes, 13},
                       {TENT_FILE_ELEMENT_PADDING, nullptr, 7}},
                      *b);
    b.reset();
    awaitPoll();
    tent_file_result_t result{};
    EXPECT_EQ(tent_file_drain(engine, id, 5000, 1, &result),
              TENT_FILE_UNQUIESCED);
    EXPECT_EQ(result.drained, 0u);
    EXPECT_EQ(result.outcome, TENT_FILE_IO_ERROR);
    EXPECT_EQ(tent_file_release_batch(engine, id), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(tent_file_close(engine, f, 0, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(counts->destroyed, 0);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 20u);
    fault->fail_poll = false;
    result = finish(id);
    EXPECT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_IO_ERROR);
    EXPECT_EQ(result.transferred_bytes, 20u);
    EXPECT_EQ(counts->destroyed, 1);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
    EXPECT_EQ(fault->submits, 1);
}

TEST_F(FileCApi, PaddingBudgetIsBoundedAndReleasedOnlyAfterDrain) {
    constexpr size_t window = 4 << 20;
    auto f = largeFile(window);
    auto b = buffer(1);
    fault->hold = true;
    for (int i = 0; i < 16; ++i)
        submitv(f, OPCODE_READ, 0,
                {{TENT_FILE_ELEMENT_PADDING, nullptr, window}}, *b);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 64u << 20);
    const tent_file_element_t element{TENT_FILE_ELEMENT_PADDING, nullptr, 1};
    auto owner = b->token();
    tent_file_batch_t rejected = 99;
    EXPECT_EQ(tent_file_submitv(engine, f, OPCODE_READ, 0, &element, 1, &owner,
                                &rejected),
              TENT_FILE_BUSY);
    EXPECT_EQ(rejected, 0u);
    EXPECT_EQ(b->counts->retained, 16);
    EXPECT_EQ(b->counts->released, 0);
    fault->hold = false;
    for (auto id : batches) EXPECT_EQ(finish(id).drained, 1u);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
    auto id = submitv(f, OPCODE_READ, 0, {element}, *b);
    EXPECT_EQ(finish(id).outcome, TENT_FILE_OK);
}

TEST_F(FileCApi, InvalidPaddingRejectsBeforeAdmission) {
    auto f = open();
    auto b = buffer(4096);
    auto owner = b->token();
    for (const auto& element : std::vector<tent_file_element_t>{
             {static_cast<tent_file_element_kind_t>(99), nullptr, 1},
             {TENT_FILE_ELEMENT_PADDING, b->bytes, 1},
             {TENT_FILE_ELEMENT_PADDING, nullptr, 0},
             {TENT_FILE_ELEMENT_PADDING, nullptr, UINT64_MAX},
             {TENT_FILE_ELEMENT_RANGE, nullptr, 1}}) {
        tent_file_batch_t id = 99;
        EXPECT_EQ(tent_file_submitv(engine, f, OPCODE_WRITE, 0, &element, 1,
                                    &owner, &id),
                  TENT_FILE_INVALID_ARGUMENT);
        EXPECT_EQ(id, 0u);
    }
    EXPECT_EQ(b->counts->retained, 0);
    EXPECT_EQ(fault->submits, 0);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
}

TEST_F(FileCApi, UnalignedGpuShimVectorsUseRealFileAndOwnedScratch) {
    auto b = buffer(4096);
    auto* data = static_cast<unsigned char*>(b->bytes);
    auto shim = std::make_shared<DeviceCopyShim>(data, 4096);
    fileIO()->setPlatformForTest(shim);
    auto f = open();
    memset(data + 1, 0x21, 17);
    memset(data + 29, 0x42, 23);
    const std::vector<tent_file_element_t> elements{
        {TENT_FILE_ELEMENT_PADDING, nullptr, 3},
        {TENT_FILE_ELEMENT_RANGE, data + 1, 17},
        {TENT_FILE_ELEMENT_PADDING, nullptr, 7},
        {TENT_FILE_ELEMENT_RANGE, data + 29, 23},
        {TENT_FILE_ELEMENT_PADDING, nullptr, 9}};
    auto write = submitv(f, OPCODE_WRITE, 5, elements, *b);
    ASSERT_EQ(finish(write).outcome, TENT_FILE_OK);
    std::vector<unsigned char> disk(59);
    ASSERT_EQ(pread(fd, disk.data(), disk.size(), 5), disk.size());
    for (size_t i = 0; i < disk.size(); ++i) {
        unsigned char expected = 0;
        if (i >= 3 && i < 20) expected = 0x21;
        if (i >= 27 && i < 50) expected = 0x42;
        ASSERT_EQ(disk[i], expected) << i;
    }
    memset(data, 0xcd, 4096);
    auto read = submitv(f, OPCODE_READ, 5, elements, *b);
    auto result = finish(read);
    ASSERT_EQ(result.outcome, TENT_FILE_OK);
    EXPECT_EQ(result.transferred_bytes, 59u);
    for (size_t i = 0; i < 4096; ++i) {
        unsigned char expected = 0xcd;
        if (i >= 1 && i < 18) expected = 0x21;
        if (i >= 29 && i < 52) expected = 0x42;
        ASSERT_EQ(data[i], expected) << i;
    }
    EXPECT_EQ(shim->gathers, 2);
    EXPECT_EQ(shim->scatters, 2);
    EXPECT_EQ(fault->submits, 2);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
}

TEST_F(FileCApi, StagedCleanupFailureRetainsScratchAndIsNeverRetried) {
    ASSERT_EXIT(
        {
            auto f = open();
            auto b = buffer(4096);
            fault->fail_free = true;
            auto id = submitv(f, OPCODE_WRITE, 3,
                              {{TENT_FILE_ELEMENT_RANGE, b->bytes, 19},
                               {TENT_FILE_ELEMENT_PADDING, nullptr, 7}},
                              *b);
            tent_file_result_t result{};
            if (tent_file_drain(engine, id, 5000, 0, &result) !=
                    TENT_FILE_UNQUIESCED ||
                result.drained)
                _exit(1);
            for (int i = 0; i < 3; ++i)
                if (tent_file_drain(engine, id, 0, 1, &result) !=
                        TENT_FILE_UNQUIESCED ||
                    result.drained)
                    _exit(2);
            if (fileIO()->stagingBytesForTest() != 26 ||
                b->counts->released != 0 || fault->frees != 1 ||
                fault->submits != 1)
                _exit(3);
            _exit(0);
        },
        testing::ExitedWithCode(0), "");
}

TEST_F(FileCApi, CancelDuringGpuShimGatherWaitsForCopyAndDoesNotDispatch) {
    auto b = buffer(4096);
    auto shim = std::make_shared<DeviceCopyShim>(b->bytes, 4096);
    shim->hold = true;
    fileIO()->setPlatformForTest(shim);
    auto f = open();
    auto id = submitv(f, OPCODE_WRITE, 1,
                      {{TENT_FILE_ELEMENT_RANGE, b->bytes, 19},
                       {TENT_FILE_ELEMENT_PADDING, nullptr, 7}},
                      *b);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!shim->entered && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(shim->entered);
    tent_file_result_t result{};
    EXPECT_EQ(tent_file_drain(engine, id, 0, 1, &result), TENT_FILE_UNQUIESCED);
    EXPECT_EQ(result.drained, 0u);
    EXPECT_EQ(b->counts->released, 0);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 26u);
    EXPECT_EQ(fault->submits, 0);
    shim->hold = false;
    result = finish(id);
    EXPECT_EQ(result.drained, 1u);
    EXPECT_EQ(result.outcome, TENT_FILE_CANCELED);
    EXPECT_EQ(result.transferred_bytes, 0u);
    EXPECT_EQ(b->counts->released, 1);
    EXPECT_EQ(fileIO()->stagingBytesForTest(), 0u);
    EXPECT_EQ(fault->submits, 0);
}

TEST_F(FileCApi, FailedGpuShimCopyRetainsBackingScratchAndCharge) {
    ASSERT_EXIT(
        {
            auto b = buffer(4096);
            auto shim = std::make_shared<DeviceCopyShim>(b->bytes, 4096);
            shim->fail = true;
            fileIO()->setPlatformForTest(shim);
            auto f = open();
            auto id = submitv(f, OPCODE_WRITE, 1,
                              {{TENT_FILE_ELEMENT_RANGE, b->bytes, 19},
                               {TENT_FILE_ELEMENT_PADDING, nullptr, 7}},
                              *b);
            tent_file_result_t result{};
            if (tent_file_drain(engine, id, 5000, 0, &result) !=
                    TENT_FILE_UNQUIESCED ||
                result.drained)
                _exit(1);
            if (tent_file_release_batch(engine, id) != TENT_FILE_UNQUIESCED ||
                fileIO()->stagingBytesForTest() != 26 ||
                b->counts->released != 0 || fault->submits != 0 ||
                shim->gathers != 1)
                _exit(2);
            _exit(0);
        },
        testing::ExitedWithCode(0), "");
}
}  // namespace
