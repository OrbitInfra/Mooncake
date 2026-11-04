// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <memory>

#include "rpc_fault_peer.h"
#include "tent/runtime/transfer_engine_impl.h"
#include "tent/transfer_engine.h"
#include "tent/transport/tcp/tcp_transport.h"

extern "C" {
int status_c_open(tent_engine_t, tent_segment_id_t*, const char*);
int status_c_info(tent_engine_t, tent_segment_id_t, tent_segment_info_t*);
int status_c_submit(tent_engine_t, tent_batch_id_t, tent_request_t*, size_t,
                    int);
int status_c_result(tent_engine_t, tent_batch_id_t, size_t, int*, int*);
}

namespace mooncake::tent {
namespace {
using namespace std::chrono_literals;

class StatusTcp : public TcpTransport {
   public:
    bool controlled = false;
    TransferStatus observation{PENDING, 0};
    Status submitTransferTasks(SubBatchRef batch,
                               const std::vector<Request>& requests) override {
        if (controlled) return Status::OK();
        return TcpTransport::submitTransferTasks(batch, requests);
    }
    Status getTransferStatus(SubBatchRef batch, int id,
                             TransferStatus& status) override {
        if (controlled) {
            status = observation;
            return Status::OK();
        }
        return TcpTransport::getTransferStatus(batch, id, status);
    }
};

class CStatus : public testing::Test {
   protected:
    std::array<char, 8192> bytes{};
    std::unique_ptr<TransferEngineImpl> engine;
    std::shared_ptr<ControlService> metadata;
    std::shared_ptr<StatusTcp> tcp;
    std::vector<tent_batch_id_t> batches;

    void SetUp() override {
        auto config = std::make_shared<Config>();
        config->set("metadata_type", "p2p");
        config->set("metadata_servers", "");
        config->set("rpc_server_hostname", "127.0.0.1");
        config->set("rpc_server_port", "0");
        config->set("enable_progress_worker", false);
        config->set("enable_auto_failover_on_poll", true);
        config->set("transports/tcp/enable", true);
        config->set("transports/tcp/max_concurrent_tasks", 1);
        for (const char* name : {"rdma", "shm", "nvlink", "mnnvl", "hp_tcp",
                                 "fabric", "io_uring", "gds"})
            config->set(std::string("transports/") + name + "/enable", false);
        engine = std::make_unique<TransferEngineImpl>(config);
        ASSERT_TRUE(engine->available());
        ASSERT_EQ(
            tent_register_memory(engine.get(), bytes.data(), bytes.size()), 0);
        metadata = std::make_shared<ControlService>("p2p", "", nullptr);
        tcp = std::make_shared<StatusTcp>();
        auto name = engine->getSegmentName();
        ASSERT_TRUE(tcp->install(name, metadata, nullptr, config).ok());
        engine->swapTransportForTest(TCP, tcp);
        ASSERT_TRUE(
            metadata->segmentManager()
                .updateLocal([&](SegmentDesc& desc) {
                    desc.type = SegmentType::Memory;
                    desc.rpc_server_addr = engine->getSegmentName();
                    BufferDesc buffer;
                    buffer.addr = reinterpret_cast<uint64_t>(bytes.data());
                    buffer.length = bytes.size();
                    std::array<char, 33> epoch{};
                    EXPECT_EQ(tent_source_epoch(engine.get(), epoch.data(),
                                                epoch.size()),
                              0);
                    buffer.source_epoch = epoch.data();
                    std::get<MemorySegmentDesc>(desc.detail)
                        .buffers.push_back(buffer);
                    return Status::OK();
                })
                .ok());
    }
    void TearDown() override {
        if (tcp) {
            EXPECT_TRUE(tcp->uninstall().ok());
        }
        for (auto batch : batches)
            EXPECT_EQ(tent_free_batch(engine.get(), batch), 0);
        if (engine) {
            EXPECT_EQ(tent_unregister_memory(engine.get(), bytes.data(),
                                             bytes.size()),
                      0);
        }
    }
    tent_batch_id_t batch(size_t capacity = 1) {
        auto id = tent_allocate_batch(engine.get(), capacity);
        batches.push_back(id);
        return id;
    }
    tent_request_t request() {
        tent_request_t r{};
        r.opcode = OPCODE_WRITE;
        r.source = bytes.data();
        r.target_id = LOCAL_SEGMENT_ID;
        r.target_offset = reinterpret_cast<uint64_t>(bytes.data() + 4096);
        r.length = 16;
        r.transport_hint = TRANSPORT_TCP;
        return r;
    }
    void poll(tent_batch_id_t id, int expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        tent_status_t status{};
        do {
            ASSERT_EQ(tent_overall_status(engine.get(), id, &status), 0);
            if (status.status != STATUS_PENDING) break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        ASSERT_EQ(status.status, expected);
    }
    void result(tent_batch_id_t id, size_t task, int code, int known) {
        int actual_code = -1, actual_known = -1;
        ASSERT_EQ(status_c_result(engine.get(), id, task, &actual_code,
                                  &actual_known),
                  0);
        EXPECT_EQ(actual_code, code);
        EXPECT_EQ(actual_known, known);
    }
};

TEST_F(CStatus, SegmentLookupReturnsConnectionErrorBeforeDispatch) {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(socket, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(
        ::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)),
        0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length),
        0);
    const auto name = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    tent_segment_id_t segment = 0;
    ASSERT_EQ(status_c_open(engine.get(), &segment, name.c_str()), 0);
    tent_segment_info_t info{};
    EXPECT_EQ(status_c_info(engine.get(), segment, &info), -134);
    ASSERT_TRUE(metadata->segmentManager()
                    .updateLocal([&](SegmentDesc& desc) {
                        desc.rpc_server_addr = name;
                        return Status::OK();
                    })
                    .ok());
    const auto id = batch();
    auto r = request();
    ASSERT_EQ(status_c_submit(engine.get(), id, &r, 1, 0), 0);
    poll(id, STATUS_FAILED);
    for (int repeat = 0; repeat < 3; ++repeat) {
        tent_status_t status{};
        EXPECT_EQ(tent_task_status(engine.get(), id, 0, &status), 0);
        EXPECT_EQ(status.status, STATUS_FAILED);
        result(id, 0, 134, 1);
        result(id, TENT_ALL_TASKS, 134, 1);
    }
    ::close(socket);
}

TEST_F(CStatus, SegmentLookupReturnsUnknownEffectAfterDispatchWithoutReplay) {
    RpcFaultPeer peer(RpcFaultPeer::Fault::Eof);
    ASSERT_TRUE(peer.available());
    ASSERT_TRUE(ControlClient::sendData(peer.address(), 0, bytes.data(), 1,
                                        std::string(32, 'e'))
                    .ok());
    tent_segment_id_t segment = 0;
    ASSERT_EQ(status_c_open(engine.get(), &segment, peer.address().c_str()), 0);
    tent_segment_info_t info{};
    EXPECT_EQ(status_c_info(engine.get(), segment, &info), -135);
    EXPECT_EQ(peer.requests.load(), 2);
    EXPECT_EQ(peer.connections.load(), 1);
}

TEST_F(CStatus, BothSubmitWrappersReturnCapacityRejection) {
    auto r = request();
    std::array<tent_request_t, 2> requests{r, r};
    for (int notify : {0, 1}) {
        const auto id = batch();
        EXPECT_EQ(status_c_submit(engine.get(), id, requests.data(),
                                  requests.size(), notify),
                  -2);
        // Rejection did not append or dispatch either task.
        result(id, TENT_ALL_TASKS, 0, 1);
    }
}

TEST_F(CStatus, EpochValidationPreservesRpcFailures) {
    auto desc = *metadata->segmentManager().getLocal();
    std::atomic<bool> lose_reply{false};
    CoroRpcAgent peer;
    ASSERT_TRUE(
        peer.registerFunction(GetSegmentDesc,
                              [&](const auto&, auto& response) {
                                  if (lose_reply)
                                      throw std::runtime_error(
                                          "injected lost descriptor reply");
                                  response = json(desc).dump();
                              })
            .ok());
    ASSERT_TRUE(
        peer.registerFunction(SubscribeSegmentUpdate, [](const auto&, auto&) {})
            .ok());
    uint16_t port = 0;
    ASSERT_TRUE(peer.start(port).ok());
    desc.name = desc.rpc_server_addr = "127.0.0.1:" + std::to_string(port);
    tent_segment_id_t segment = 0;
    ASSERT_EQ(status_c_open(engine.get(), &segment, desc.name.c_str()), 0);
    const auto epoch = desc.getMemory().buffers.front().source_epoch;
    lose_reply = true;
    EXPECT_EQ(tent_validate_remote_epoch(engine.get(), segment, epoch.c_str()),
              -135);
    ASSERT_TRUE(peer.stop().ok());
    EXPECT_EQ(tent_validate_remote_epoch(engine.get(), segment, epoch.c_str()),
              -134);
}

TEST_F(CStatus, DataLossKeepsUnknownEffectAcrossTaskAndBatchPolls) {
    for (auto fault :
         {RpcFaultPeer::Fault::Eof, RpcFaultPeer::Fault::Reset,
          RpcFaultPeer::Fault::ShortHeader, RpcFaultPeer::Fault::ShortBody}) {
        for (int opcode : {OPCODE_READ, OPCODE_WRITE}) {
            SCOPED_TRACE(opcode);
            SCOPED_TRACE(static_cast<int>(fault));
            RpcFaultPeer peer(fault);
            ASSERT_TRUE(peer.available());
            ASSERT_TRUE(metadata->segmentManager()
                            .updateLocal([&](SegmentDesc& desc) {
                                desc.rpc_server_addr = peer.address();
                                return Status::OK();
                            })
                            .ok());
            const auto id = batch(2);
            auto r = request();
            ASSERT_EQ(status_c_submit(engine.get(), id, &r, 1, 0), 0);
            poll(id, STATUS_COMPLETED);
            result(id, 0, 0, 1);
            r.opcode = opcode;
            ASSERT_EQ(status_c_submit(engine.get(), id, &r, 1, 0), 0);
            poll(id, STATUS_FAILED);
            for (int repeat = 0; repeat < 3; ++repeat) {
                tent_status_t status{};
                ASSERT_EQ(tent_task_status(engine.get(), id, 1, &status), 0);
                EXPECT_EQ(status.status, STATUS_FAILED);
                result(id, 1, 135, 0);
                result(id, TENT_ALL_TASKS, 135, 0);
            }
            EXPECT_EQ(peer.requests.load(), 2);
            EXPECT_EQ(peer.connections.load(), 1);
        }
    }
}

TEST_F(CStatus, ResultIsObservationalAndPreservesNativeCodeAndAliases) {
    tcp->controlled = true;
    const auto id = batch(2);
    auto r = request();
    auto next = r;
    next.source = bytes.data() + 16;
    next.target_offset += 16;
    std::array<tent_request_t, 2> requests{r, next};
    ASSERT_EQ(
        status_c_submit(engine.get(), id, requests.data(), requests.size(), 0),
        0);
    int code = -1, known = -1;
    EXPECT_EQ(status_c_result(engine.get(), id, TENT_ALL_TASKS, &code, &known),
              -2);
    EXPECT_EQ(code, 135);
    EXPECT_EQ(known, 0);
    tcp->observation = {FAILED, 0, Status::Code::kRpcUnknownEffect, false};
    // The result query does not even poll the backend.
    EXPECT_EQ(status_c_result(engine.get(), id, 0, &code, &known), -2);
    poll(id, STATUS_FAILED);
    result(id, 0, 135, 0);
    result(id, 1, 135, 0);
    result(id, TENT_ALL_TASKS, 135, 0);
}

TEST_F(CStatus, CancelAndTimeoutWithoutReceiptStayUnknown) {
    tcp->controlled = true;
    for (auto state : {CANCELED, TIMEOUT}) {
        const auto id = batch();
        auto r = request();
        ASSERT_EQ(status_c_submit(engine.get(), id, &r, 1, 0), 0);
        tcp->observation = {state, 0};
        poll(id, static_cast<int>(state));
        result(id, TENT_ALL_TASKS, 135, 0);
    }
}

TEST_F(CStatus, InvalidOrFreedReceiptDoesNotReturnSuccess) {
    int code = 0, known = 1;
    EXPECT_EQ(status_c_result(engine.get(), 0, 0, &code, &known), -1);
    EXPECT_EQ(code, 135);
    EXPECT_EQ(known, 0);
    const auto id = batch();
    EXPECT_EQ(status_c_result(engine.get(), id, 0, &code, &known), -1);
    ASSERT_EQ(tent_free_batch(engine.get(), id), 0);
    batches.clear();
    EXPECT_EQ(status_c_result(engine.get(), id, TENT_ALL_TASKS, &code, &known),
              -1);
}
}  // namespace
}  // namespace mooncake::tent
