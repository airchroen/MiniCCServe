#pragma once

// HTTP server: TCP accept loop plus one worker thread per connection.

#include "common.hpp"
#include "router.hpp"
#include "types.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

class HttpServer {
public:
    using LogFn = std::function<void(LogEntry)>;

    // Upper bound on simultaneously served connections. Extra connections
    // receive an immediate 503 response.
    static constexpr int kMaxConnections = 64;

    ~HttpServer();

    HttpServer() = default;
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Starts listening on `address`:`port`. Returns false and fills `error`
    // with a user-readable message on failure (invalid address, port in
    // use, ...).
    bool Start(int port, const std::string& address, Router& router, std::string& error);

    // Stops the accept loop, interrupts all in-flight connections and joins
    // every worker thread.
    void Stop();

    bool Running() const { return running_.load(); }

    void SetLogFn(LogFn fn) { log_ = std::move(fn); }

private:
    struct ClientState {
        SOCKET socket = INVALID_SOCKET;
        std::atomic<bool> done{false};
        std::unique_ptr<std::jthread> thread;
    };

    void AcceptLoop();
    void HandleClient(const std::shared_ptr<ClientState>& state);
    void Respond(SOCKET s, const HttpRequest& req, const HttpResponse& resp);
    void PruneFinishedClients();

    std::jthread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_{0};
    SOCKET listen_ = INVALID_SOCKET;
    Router* router_ = nullptr;
    LogFn log_;

    std::mutex clientsMx_;
    std::vector<std::shared_ptr<ClientState>> clients_;
};
