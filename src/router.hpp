#pragma once

// Request dispatch: user-defined mock routes (routes.json) first, then static
// file serving from the configured root directory.

#include "types.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct MockRoute {
    std::string method = "GET";
    std::string path;                                        // exact match
    int status = 200;                                        // 100-599
    int delayMs = 0;                                         // artificial latency
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

class Router {
public:
    // (Re)loads mock routes from a JSON file. On any error the previously
    // loaded routes stay active and `error` carries a readable message.
    bool LoadRoutes(const std::string& file, std::string& error);

    void SetRoot(const std::string& dir);
    const std::string Root() const;

    size_t RouteCount() const;

    // Thread safe: takes a snapshot of the current config under a lock, then
    // serves without holding it.
    HttpResponse Handle(const HttpRequest& req);

private:
    std::optional<MockRoute> MatchMock(const HttpRequest& req);

    std::mutex mx_;                 // guards routes_ and root_
    std::vector<MockRoute> routes_;
    std::string root_;
};
