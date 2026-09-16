#pragma once

// Data types shared between the HTTP layer, the router and the GUI.

#include <cstddef>
#include <string>

struct HttpRequest {
    std::string method;  // "GET", "POST", ... (as sent by the client)
    std::string path;    // percent-decoded path, always starts with '/'
    std::string query;   // raw query string without the leading '?'
    std::string body;    // request body (only Content-Length bodies are read)
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
};

// Bodies kept per log row for the double-click viewer. Anything larger is
// stored truncated (the full size is still recorded).
constexpr size_t kMaxLoggedBodyBytes = 16 * 1024;

// One row in the GUI request log.
struct LogEntry {
    std::string time;        // "HH:MM:SS" local time
    std::string method;
    std::string path;
    std::string query;       // raw query string, no leading '?'
    int status = 0;
    long long durationMs = 0;
    std::string reqContentType;  // request Content-Type header, may be empty
    std::string respContentType; // response Content-Type header
    size_t reqBodySize = 0;      // full request body size in bytes
    size_t respBodySize = 0;     // full response body size in bytes
    std::string reqBody;         // request body, truncated to kMaxLoggedBodyBytes
    std::string respBody;        // response body, truncated to kMaxLoggedBodyBytes
};

// Reason phrase for common status codes; "Response" for anything else.
const char* StatusReason(int status);
