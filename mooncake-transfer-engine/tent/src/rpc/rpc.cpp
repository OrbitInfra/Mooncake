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
#include "tent/rpc/rpc.h"

#include <glog/logging.h>
#include <async_simple/executors/SimpleExecutor.h>

#include <stdexcept>
#include <cerrno>
#include <algorithm>
#include <sys/socket.h>

#include "transfer_engine_rpc_client_io_context.h"
#include "tent/common/utils/ip.h"
#include "tent/common/utils/random.h"

namespace mooncake {
namespace tent {

using namespace coro_rpc;
using namespace async_simple::coro;

namespace {
thread_local bool tl_inside_rpc_handler = false;

struct RpcHandlerScope {
    explicit RpcHandlerScope(bool& flag) : flag_(flag), prev_(flag) {
        flag_ = true;
    }
    ~RpcHandlerScope() { flag_ = prev_; }

   private:
    bool& flag_;
    bool prev_;
};
}  // namespace

class ClientPool {
   public:
    explicit ClientPool(size_t limit) : limit_(limit) {
        // Lease destruction must not allocate while returning a connection.
        idle_clients_.reserve(limit);
    }
    struct Lease {
        std::unique_ptr<coro_rpc_client> client;
        uint64_t generation = 0;
        bool admitted = false;
    };

    Lease acquire() {
        std::lock_guard<std::mutex> guard(lock_);
        if (active_ >= limit_) return {};
        ++active_;
        if (idle_clients_.empty()) return {nullptr, generation_, true};
        auto client = std::move(idle_clients_.back());
        idle_clients_.pop_back();
        return {std::move(client), generation_, true};
    }

    void release(std::unique_ptr<coro_rpc_client> client, uint64_t generation,
                 bool broken) {
        std::lock_guard<std::mutex> guard(lock_);
        --active_;
        if (client && !broken && !client->has_closed() &&
            generation == generation_ &&
            active_ + idle_clients_.size() < limit_) {
            idle_clients_.push_back(std::move(client));
        }
        // Otherwise destruction closes the client. A lease from a flushed
        // generation must never repopulate the replacement pool.
    }

    void clearIfCurrent(uint64_t generation) {
        std::lock_guard<std::mutex> guard(lock_);
        if (generation != generation_) return;
        ++generation_;
        idle_clients_.clear();
    }

    void clear() {
        std::lock_guard<std::mutex> guard(lock_);
        ++generation_;
        idle_clients_.clear();
    }

    size_t size() {
        std::lock_guard<std::mutex> guard(lock_);
        return active_ + idle_clients_.size();
    }

   private:
    std::mutex lock_;
    const size_t limit_;
    size_t active_ = 0;
    uint64_t generation_ = 0;
    std::vector<std::unique_ptr<coro_rpc_client>> idle_clients_;
};

struct ClientLease {
    std::unique_ptr<coro_rpc_client> client;
    std::shared_ptr<ClientPool> pool;
    uint64_t generation;
    // Exceptions and incomplete replies retire the socket by default.
    bool broken = true;

    ~ClientLease() { pool->release(std::move(client), generation, broken); }

    coro_rpc_client* operator->() const { return client.get(); }
};

CoroRpcAgent::CoroRpcAgent() = default;

CoroRpcAgent::~CoroRpcAgent() { stop(); }

Status CoroRpcAgent::registerFunction(int func_id, const Function& func,
                                      bool offload) {
    std::lock_guard<std::mutex> guard(func_map_mutex_);
    func_map_[func_id] = Handler{func, offload};
    return Status::OK();
}

Status CoroRpcAgent::start(uint16_t& port, bool ipv6, size_t threads) {
    const static uint16_t kStartPort = 15000;
    const static uint16_t kPortRange = 2000;
    const static int kMaxRetry = 10;
    if (running_)
        return Status::InvalidArgument("RPC server already started" LOC_MARK);
    easylog::set_min_severity(easylog::Severity::FATAL);
    if (threads > 1)
        LOG(INFO) << "CoroRpcAgent: RPC server threads set to " << threads;
    for (int retry = 0; retry < kMaxRetry; ++retry) {
        try {
            if (port == 0)
                port = kStartPort + SimpleRandom::Get().next(kPortRange);
            server_ = new coro_rpc::coro_rpc_server(threads, port,
                                                    ipv6 ? "::" : "0.0.0.0");
            server_->register_handler<&CoroRpcAgent::process>(this);
            server_->async_start();
            const auto err = server_->get_errc();
            if (err) {
                LOG(WARNING)
                    << "Failed to start RPC server(async_start) on port "
                    << port << ": " << err.message();
                delete server_;
                server_ = nullptr;
                port = 0;
                continue;
            }
            {
                std::lock_guard<std::mutex> guard(dispatch_mutex_);
                dispatch_closed_ = false;
            }
            running_ = true;
            return Status::OK();
        } catch (const std::exception& e) {
            LOG(WARNING) << "Failed to start RPC server on port " << port
                         << ": " << e.what();
            port = 0;
        }
    }
    return Status::RpcServiceError("Failed to bind any RPC port" LOC_MARK);
}

uint64_t CoroRpcAgent::closeDispatch(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(dispatch_mutex_);
    dispatch_closed_ = true;
    dispatch_cv_.wait_for(guard, timeout,
                          [this] { return dispatch_active_ == 0; });
    return dispatch_active_;
}

void CoroRpcAgent::finishDispatch() {
    std::lock_guard<std::mutex> guard(dispatch_mutex_);
    --dispatch_active_;
    dispatch_cv_.notify_all();
}

Status CoroRpcAgent::stop() {
    {
        std::unique_lock<std::mutex> guard(dispatch_mutex_);
        dispatch_closed_ = true;
        // Keep I/O alive until queued global-executor handlers resume and
        // finish.
        dispatch_cv_.wait(guard, [this] { return dispatch_active_ == 0; });
    }
    if (server_) {
        server_->stop();
        delete server_;
        server_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(pools_mutex_);
    for (auto& entry : pools_) {
        entry.second->clear();
    }
    pools_.clear();
    return Status::OK();
}

Lazy<void> CoroRpcAgent::process(int func_id) {
    auto* ctx = co_await coro_rpc::get_context_in_coro();
    {
        std::lock_guard<std::mutex> guard(dispatch_mutex_);
        if (dispatch_closed_ || dispatch_active_ == UINT64_MAX) {
            ctx->set_response_attachment(
                std::string("RPC dispatch admission closed"));
            co_return;
        }
        ++dispatch_active_;
    }
    DispatchGuard dispatch_guard(*this);
    // registerFunction() may run while the server is serving, and a rehash
    // under a bare find() is a data race. The handler is copied out and the
    // lock released before the coroutine can suspend below.
    Handler handler;
    {
        std::lock_guard<std::mutex> guard(func_map_mutex_);
        auto it = func_map_.find(func_id);
        if (it == func_map_.end()) {
            throw std::runtime_error("Unknown TENT RPC function: " +
                                     std::to_string(func_id));
        }
        handler = it->second;
    }

    auto request = ctx->get_request_attachment();
    std::string response;
    if (handler.offload) {
        // Suspends this coroutine, freeing the io_context thread. The request
        // buffer belongs to the context_info this coroutine keeps alive, so
        // the view survives the hop. tl_inside_rpc_handler is deliberately
        // not set: it guards against deadlocking the RPC thread, which an
        // offloaded handler no longer occupies.
        //
        // post() reports a throwing handler through the Try instead of
        // unwinding, so rethrow here: the router turns that into the same
        // rpc_throw_exception an inline handler would produce. Dropping it
        // would answer a malformed request with an empty success.
        auto result =
            co_await coro_io::post([&] { handler.func(request, response); });
        result.value();
    } else {
        RpcHandlerScope handler_scope(tl_inside_rpc_handler);
        handler.func(request, response);
    }
    ctx->set_response_attachment(std::move(response));
}

std::shared_ptr<ClientPool> CoroRpcAgent::getOrCreatePool(
    const std::string& server_addr) {
    std::lock_guard<std::mutex> lock(pools_mutex_);
    auto it = pools_.find(server_addr);
    if (it != pools_.end()) return it->second;
    if (pools_.size() >= max_cached_peers_) {
        // Evict only a cache-owned pool. Active calls pin their pool, including
        // its connection budget, until completion; never create a second pool
        // for an address whose first pool still has owners.
        auto idle = std::find_if(
            pools_.begin(), pools_.end(),
            [](const auto& entry) { return entry.second.use_count() == 1; });
        if (idle == pools_.end()) return nullptr;
        pools_.erase(idle);
    }
    return pools_
        .emplace(server_addr,
                 std::make_shared<ClientPool>(max_connections_per_peer_))
        .first->second;
}

std::pair<size_t, size_t> CoroRpcAgent::poolUsageForTest() {
    std::lock_guard<std::mutex> lock(pools_mutex_);
    size_t connections = 0;
    for (const auto& entry : pools_) connections += entry.second->size();
    return {pools_.size(), connections};
}

namespace {

// True when the failure came back from the peer as a reply, which means the
// connection carried a full round trip and is still usable. Every other
// failure leaves the connection in an unknown state.
bool peerAnswered(coro_rpc::errc ec) {
    switch (ec) {
        case coro_rpc::errc::rpc_throw_exception:
        case coro_rpc::errc::function_not_registered:
        case coro_rpc::errc::invalid_rpc_arguments:
            return true;
        default:
            return false;
    }
}

// The lease is exclusive. Inspect on the socket's executor so timer/close
// callbacks cannot race native_handle(), and never consume framing bytes.
// This is only a pre-send optimization: a peer can die immediately afterward.
bool idleConnectionUsable(coro_rpc_client& client) {
    if (client.has_closed()) return false;
    auto* socket = client.tcp_socket_for_idle_check();
    if (!socket || !socket->is_open()) return false;
    char byte;
    const auto count =
        ::recv(socket->native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    // FIN, reset, any other socket error, or unexpected pending frame bytes
    // retire the idle stream. Only would-block establishes "no known failure".
    return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

std::chrono::milliseconds remaining(RpcDeadline deadline) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
}

}  // namespace

Lazy<std::pair<Status, std::string>> CoroRpcAgent::callCoroutine(
    std::string server_addr, int func_id, std::string request,
    RpcDeadline deadline) {
    if (tl_inside_rpc_handler) {
        co_return std::make_pair(
            Status::InvalidArgument(
                "RPC call from RPC handler is forbidden" LOC_MARK),
            "");
    }

    // One budget covers stale-socket replacement and failed connects. There
    // is no replay once call_for() starts: YLT does not expose the number of
    // request bytes written, so even EPIPE must conservatively be uncertain.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (remaining(deadline).count() <= 0)
            co_return std::make_pair(
                Status::RpcConnectionError(
                    "RPC deadline expired before request was sent" LOC_MARK),
                "");
        auto pool = getOrCreatePool(server_addr);
        if (!pool)
            co_return std::make_pair(
                Status::TooManyRequests(
                    "RPC peer pool capacity exhausted" LOC_MARK),
                "");
        auto acquired = pool->acquire();
        if (!acquired.admitted)
            co_return std::make_pair(
                Status::TooManyRequests(
                    "RPC connection pool capacity exhausted" LOC_MARK),
                "");
        const auto generation = acquired.generation;
        ClientLease lease{std::move(acquired.client), pool, generation};
        if (!lease.client) {
            lease.client = std::make_unique<coro_rpc_client>(
                GetTransferEngineRpcClientIoContextPool().get_executor());
            auto connect_budget = remaining(deadline);
            if (connect_budget.count() <= 0)
                co_return std::make_pair(
                    Status::RpcConnectionError(
                        "RPC deadline expired before connect" LOC_MARK),
                    "");
            auto conn_result =
                co_await lease->connect(server_addr, connect_budget);
            if (conn_result.val() != 0) {
                pool->clearIfCurrent(generation);
                if (attempt == 0 && remaining(deadline).count() > 0) continue;
                co_return std::make_pair(
                    Status::RpcConnectionError(
                        "RPC request not sent: connect failed. server: " +
                        server_addr + ", message: " +
                        std::string(conn_result.message()) + LOC_MARK),
                    "");
            }
        }

        auto usable = co_await coro_io::post(
            [&lease] { return idleConnectionUsable(*lease.client); },
            &lease->get_executor());
        if (!usable.value()) {
            pool->clearIfCurrent(generation);
            if (attempt == 0 && remaining(deadline).count() > 0) continue;
            co_return std::make_pair(
                Status::RpcConnectionError("RPC request not sent: connection "
                                           "closed before send" LOC_MARK),
                "");
        }
        if (before_send_for_test_) before_send_for_test_();
        auto budget = remaining(deadline);
        if (budget.count() <= 0)
            co_return std::make_pair(
                Status::RpcConnectionError(
                    "RPC deadline expired before request was sent" LOC_MARK),
                "");
        lease->set_req_attachment(request);
        auto call_result =
            co_await lease->call_for<&CoroRpcAgent::process>(budget, func_id);
        if (!call_result.has_value()) {
            const auto& error = call_result.error();
            const auto msg = "server: " + server_addr +
                             ", func_id: " + std::to_string(func_id) +
                             ", message: " + std::string(error.msg);
            // EOF, reset, EPIPE, timeout and partial/invalid frames all retire
            // the client and its idle generation. None proves non-delivery.
            if (!peerAnswered(error.code)) pool->clearIfCurrent(generation);
            // Even a replied handler exception can follow a remote side
            // effect. Application rejections in a successful response retain
            // their own status (including the source-epoch rejection).
            co_return std::make_pair(
                Status::RpcUnknownEffect(
                    "RPC outcome is unknown; request may have reached peer. " +
                    msg + LOC_MARK),
                "");
        }

        const size_t len = lease->get_resp_attachment().size();
        std::string response = lease->release_resp_attachment();
        response.resize(len);
        lease.broken = false;
        co_return std::make_pair(Status::OK(), std::move(response));
    }
    co_return std::make_pair(
        Status::RpcConnectionError(
            "RPC pre-send retry budget exhausted" LOC_MARK),
        "");
}

Status CoroRpcAgent::call(const std::string& server_addr, int func_id,
                          const std::string_view& request,
                          std::string& response, RpcDeadline deadline) {
    auto [status, resp] = async_simple::coro::syncAwait(
        callCoroutine(server_addr, func_id, std::string(request), deadline));
    if (status.ok()) response = std::move(resp);
    return status;
}

Status CoroRpcAgent::callOwned(const std::string& server_addr, int func_id,
                               std::string request, std::string& response,
                               RpcDeadline deadline) {
    auto [status, resp] = async_simple::coro::syncAwait(
        callCoroutine(server_addr, func_id, std::move(request), deadline));
    if (status.ok()) response = std::move(resp);
    return status;
}

void CoroRpcAgent::callAsync(const std::string& server_addr, int func_id,
                             const std::string& request, AsyncCallback callback,
                             RpcDeadline deadline) {
    callCoroutine(server_addr, func_id, request, deadline)
        .start([cb = std::move(callback)](auto&& try_result) {
            if (try_result.hasError()) {
                cb(Status::RpcUnknownEffect(
                       "Async RPC exception; outcome unknown" LOC_MARK),
                   "");
            } else {
                auto& val = try_result.value();
                cb(val.first, std::move(val.second));
            }
        });
}

void CoroRpcAgent::callAsyncRetryOnce(const std::string& server_addr,
                                      int func_id, const std::string& request,
                                      AsyncCallback callback) {
    callAsync(server_addr, func_id, request, std::move(callback));
}
}  // namespace tent
}  // namespace mooncake
