// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "tent/runtime/file_io.h"
#include "tent/runtime/transfer_engine_impl.h"
#include "tent/transfer_engine.h"

using namespace mooncake::tent;
using namespace std::chrono_literals;
extern "C" int file_c_api_read(tent_engine_t, tent_file_id_t,
                               tent_file_range_t*, size_t,
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

// All I/O goes through the installed io_uring transport. Only completion
// observation or the cleanup return is injected; no fake byte movement.
class CompletionFault : public Transport {
   public:
    std::shared_ptr<Transport> real;
    std::atomic<bool> hold{false}, fail_poll{false}, fail_free{false};
    std::atomic<bool> hold_close{false}, fail_close{false};
    std::atomic<int> closes{0};
    std::atomic<int> polls{0}, submits{0}, frees{0};
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
        return real->submitTransferTasks(b, r);
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
        const char* directory = std::getenv("TENT_FILE_TEST_DIR");
        const std::string pattern =
            std::string(directory ? directory : "/dev/shm") +
            "/tent-file-c-XXXXXX";
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
    void awaitPoll() {
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (!fault->polls && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(1ms);
        ASSERT_GT(fault->polls, 0);
    }
};

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
}  // namespace
