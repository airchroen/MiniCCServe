#include "http.hpp"

#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

namespace {

constexpr size_t kMaxHeaderBytes = 32 * 1024;
constexpr size_t kMaxBodyBytes = 10 * 1024 * 1024;
constexpr int kRecvTimeoutSec = 15;

// Minimal RAII owner for a socket handle.
class ScopedSocket {
public:
    explicit ScopedSocket(SOCKET s = INVALID_SOCKET) : s_(s) {}
    ~ScopedSocket() { Reset(); }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;
    ScopedSocket(ScopedSocket&& o) noexcept : s_(o.s_) { o.s_ = INVALID_SOCKET; }
    ScopedSocket& operator=(ScopedSocket&& o) noexcept
    {
        if (this != &o) {
            Reset();
            s_ = o.s_;
            o.s_ = INVALID_SOCKET;
        }
        return *this;
    }
    void Reset(SOCKET s = INVALID_SOCKET)
    {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = s;
    }
    SOCKET Get() const { return s_; }

private:
    SOCKET s_;
};

std::string NowTimeOfDay()
{
    char buf[16];
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return buf;
}

bool SendAll(SOCKET s, const char* data, size_t len)
{
    while (len > 0) {
        int n = send(s, data, (int)std::min(len, size_t{1} << 20), 0);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool PercentDecode(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;
            int hi = HexVal(in[i + 1]);
            int lo = HexVal(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(char((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return true;
}

// A decoded path is safe to map under the root when every segment is free of
// traversal and drive-letter tricks.
bool PathIsSafe(const std::string& path)
{
    if (path.empty() || path[0] != '/') return false;
    size_t start = 1;
    for (;;) {
        size_t end = path.find('/', start);
        std::string seg = path.substr(start, end == std::string::npos ? end : end - start);
        if (seg == ".." || seg == ".") return false;
        for (char c : seg) {
            if (c == '\\' || c == ':' || c == '<' || c == '>' || c == '"') return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string LowerCopy(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Content-Length from the header block, or -1 when absent, -2 when malformed.
long long ContentLengthFromHeaders(const std::string& head)
{
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        size_t lineStart = pos + 2;
        size_t lineEnd = head.find("\r\n", lineStart);
        std::string line = head.substr(lineStart, lineEnd == std::string::npos ? lineEnd : lineEnd - lineStart);
        if (line.empty()) break;  // end of headers
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = LowerCopy(Trim(line.substr(0, colon)));
            if (name == "content-length") {
                long long v = atoll(Trim(line.substr(colon + 1)).c_str());
                return v < 0 ? -2 : v;
            }
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd;
    }
    return -1;
}

// Header value by (case-insensitive) name from the header block, "" if absent.
std::string HeaderValue(const std::string& head, const char* name)
{
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        size_t lineStart = pos + 2;
        size_t lineEnd = head.find("\r\n", lineStart);
        std::string line = head.substr(lineStart, lineEnd == std::string::npos ? lineEnd : lineEnd - lineStart);
        if (line.empty()) break;  // end of headers
        size_t colon = line.find(':');
        if (colon != std::string::npos && LowerCopy(Trim(line.substr(0, colon))) == name)
            return Trim(line.substr(colon + 1));
        if (lineEnd == std::string::npos) break;
        pos = lineEnd;
    }
    return "";
}

// Body kept for the log viewer: capped, with a marker when the tail is cut.
std::string TruncatedBody(const std::string& body)
{
    if (body.size() <= kMaxLoggedBodyBytes) return body;
    return body.substr(0, kMaxLoggedBodyBytes) + "\n... (truncated)";
}

// Assemble the log row from what was exchanged. Bodies are copied (capped);
// sizes record the full length so the viewer can say "truncated".
LogEntry MakeLogEntry(const HttpRequest& req, const std::string& reqContentType,
                      const HttpResponse& resp, long long durationMs)
{
    LogEntry e;
    e.time = NowTimeOfDay();
    e.method = req.method.empty() ? "?" : req.method;
    e.path = req.path.empty() ? "?" : req.path;
    e.query = req.query;
    e.status = resp.status;
    e.durationMs = durationMs;
    e.reqContentType = reqContentType;
    e.respContentType = resp.contentType;
    e.reqBodySize = req.body.size();
    e.respBodySize = resp.body.size();
    e.reqBody = TruncatedBody(req.body);
    e.respBody = TruncatedBody(resp.body);
    return e;
}

}  // namespace

const char* StatusReason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Response";
    }
}

HttpServer::~HttpServer()
{
    Stop();
}

bool HttpServer::Start(int port, const std::string& address, Router& router, std::string& error)
{
    if (running_.load()) return true;
    router_ = &router;

    in_addr bindAddr{};
    if (inet_pton(AF_INET, address.c_str(), &bindAddr) != 1) {
        error = "\"" + address + "\" is not a valid IPv4 address.";
        return false;
    }

    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) {
        error = "socket() failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof reuse);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr = bindAddr;
    addr.sin_port = htons((u_short)port);

    if (bind(listen_, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR) {
        int wsa = WSAGetLastError();
        error = address + ":" + std::to_string(port) +
                (wsa == WSAEADDRINUSE ? " is already in use by another program."
                                      : " could not be bound (WSA error " + std::to_string(wsa) + ").");
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_, SOMAXCONN) == SOCKET_ERROR) {
        error = "listen() failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }

    running_.store(true);
    acceptThread_ = std::jthread([this] { AcceptLoop(); });
    return true;
}

void HttpServer::Stop()
{
    if (!running_.exchange(false) && !acceptThread_.joinable() && clients_.empty()) return;

    if (acceptThread_.joinable()) acceptThread_.join();

    // Wake every blocked worker (recv fails immediately), then join them all.
    std::vector<std::shared_ptr<ClientState>> snapshot;
    {
        std::lock_guard<std::mutex> lk(clientsMx_);
        snapshot = clients_;
    }
    for (auto& st : snapshot) {
        SOCKET s = st->socket;
        if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    }
    for (auto& st : snapshot) {
        if (st->thread && st->thread->joinable()) st->thread->join();
    }
    {
        std::lock_guard<std::mutex> lk(clientsMx_);
        clients_.clear();
    }

    if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
}

void HttpServer::AcceptLoop()
{
    while (running_.load()) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(listen_, &rf);
        timeval tv{1, 0};
        int r = select((int)listen_ + 1, &rf, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        sockaddr_in peer{};
        int peerLen = sizeof peer;
        SOCKET c = accept(listen_, (sockaddr*)&peer, &peerLen);
        if (c == INVALID_SOCKET) continue;

        if (active_.load() >= kMaxConnections) {
            std::string body = "Too many concurrent connections";
            std::string head = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "Connection: close\r\n\r\n";
            SendAll(c, head.data(), head.size());
            SendAll(c, body.data(), body.size());
            closesocket(c);
            continue;
        }

        auto state = std::make_shared<ClientState>();
        state->socket = c;
        state->thread = std::make_unique<std::jthread>([this, state] {
            HandleClient(state);
            state->done.store(true);
            active_.fetch_sub(1);
        });
        active_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(clientsMx_);
            clients_.push_back(state);
        }
        PruneFinishedClients();
    }
}

void HttpServer::HandleClient(const std::shared_ptr<ClientState>& state)
{
    ScopedSocket s(state->socket);
    auto t0 = std::chrono::steady_clock::now();

    DWORD timeout = kRecvTimeoutSec * 1000;
    setsockopt(s.Get(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);

    std::string raw;
    size_t headerEnd = std::string::npos;
    char chunk[4096];
    while (headerEnd == std::string::npos) {
        int n = recv(s.Get(), chunk, sizeof chunk, 0);
        if (n <= 0) return;  // client vanished before sending a full header
        raw.append(chunk, (size_t)n);
        headerEnd = raw.find("\r\n\r\n");
        if (raw.size() > kMaxHeaderBytes) {
            HttpResponse r{431, StatusReason(431), "text/plain; charset=utf-8", "Header block too large."};
            Respond(s.Get(), HttpRequest{}, r);
            if (log_) log_(MakeLogEntry(HttpRequest{}, "", r, 0));
            return;
        }
    }

    std::string headerBlock = raw.substr(0, headerEnd);

    // --- Parse request line ---
    HttpRequest req;
    size_t lineEnd = headerBlock.find("\r\n");
    std::string firstLine = headerBlock.substr(0, lineEnd);
    size_t sp1 = firstLine.find(' ');
    size_t sp2 = firstLine.rfind(' ');

    // Request Content-Type is captured for the log viewer.
    const std::string reqContentType = HeaderValue(headerBlock, "content-type");

    auto sendError = [&](int status, const char* text) {
        HttpResponse r{status, StatusReason(status), "text/plain; charset=utf-8", text};
        Respond(s.Get(), req, r);
        if (log_) log_(MakeLogEntry(req, reqContentType, r, 0));
    };

    if (sp1 == std::string::npos || sp2 == sp1 || sp2 == std::string::npos) {
        req.method = "?";
        req.path = "?";
        sendError(400, "Malformed request line.");
        return;
    }

    req.method = firstLine.substr(0, sp1);
    std::string target = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/' || req.method.empty()) {
        req.path = target.empty() ? "?" : target;
        sendError(400, "Malformed request line.");
        return;
    }

    size_t q = target.find('?');
    if (q != std::string::npos) {
        req.path = target.substr(0, q);
        req.query = target.substr(q + 1);
    } else {
        req.path = target;
    }

    std::string decoded;
    if (!PercentDecode(req.path, decoded) || !PathIsSafe(decoded)) {
        sendError(400, "Invalid request path.");
        return;
    }
    req.path = decoded;

    // --- Body ---
    long long clen = ContentLengthFromHeaders(headerBlock);
    if (clen == -2) {
        sendError(400, "Invalid Content-Length.");
        return;
    }
    if (clen > (long long)kMaxBodyBytes) {
        sendError(413, "Request body too large.");
        return;
    }
    if (clen > 0) {
        req.body = raw.substr(headerEnd + 4);
        if (req.body.size() > (size_t)clen) req.body.resize((size_t)clen);
        while (req.body.size() < (size_t)clen) {
            int n = recv(s.Get(), chunk, (int)std::min(sizeof chunk, (size_t)clen - req.body.size()), 0);
            if (n <= 0) return;  // client vanished mid-body
            req.body.append(chunk, (size_t)n);
        }
    }

    // --- Dispatch ---
    HttpResponse resp = router_->Handle(req);
    Respond(s.Get(), req, resp);

    if (log_) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        log_(MakeLogEntry(req, reqContentType, resp, ms));
    }
}

void HttpServer::Respond(SOCKET s, const HttpRequest& req, const HttpResponse& resp)
{
    std::string head;
    head.reserve(256);
    head += "HTTP/1.1 " + std::to_string(resp.status) + " " +
            (resp.reason.empty() ? StatusReason(resp.status) : resp.reason) + "\r\n";
    head += "Content-Type: " + resp.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    head += "Connection: close\r\n";
    head += "Server: MiniCCServe\r\n";
    head += "\r\n";

    if (!SendAll(s, head.data(), head.size())) return;
    if (req.method != "HEAD") SendAll(s, resp.body.data(), resp.body.size());
}

void HttpServer::PruneFinishedClients()
{
    std::vector<std::shared_ptr<ClientState>> finished;
    {
        std::lock_guard<std::mutex> lk(clientsMx_);
        std::erase_if(clients_, [&](const std::shared_ptr<ClientState>& st) {
            if (st->done.load()) {
                finished.push_back(st);
                return true;
            }
            return false;
        });
    }
    for (auto& st : finished) {
        if (st->thread && st->thread->joinable()) st->thread->join();
    }
}
