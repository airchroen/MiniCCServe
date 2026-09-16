#include "router.hpp"

#include "util.hpp"

#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <windows.h>

namespace {

constexpr size_t kMaxStaticFileBytes = 100 * 1024 * 1024;

struct DirEntry {
    std::string name;
    bool isDir = false;
    unsigned long long size = 0;
};

std::string LowerCopyExt(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string ContentTypeFor(const std::string& path)
{
    size_t dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : LowerCopyExt(path.substr(dot + 1));
    static const struct { const char* ext; const char* type; } kMap[] = {
        {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},   {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"}, {"md", "text/plain; charset=utf-8"},
        {"csv", "text/plain; charset=utf-8"},
        {"xml", "application/xml; charset=utf-8"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"webp", "image/webp"},
        {"bmp", "image/bmp"}, {"ico", "image/x-icon"},
        {"pdf", "application/pdf"}, {"zip", "application/zip"},
        {"gz", "application/gzip"},
        {"woff", "font/woff"}, {"woff2", "font/woff2"},
        {"ttf", "font/ttf"}, {"otf", "font/otf"},
        {"mp4", "video/mp4"}, {"webm", "video/webm"},
        {"mp3", "audio/mpeg"}, {"wav", "audio/wav"},
    };
    for (const auto& m : kMap)
        if (ext == m.ext) return m.type;
    return "application/octet-stream";
}

std::string HumanSize(unsigned long long bytes)
{
    char buf[32];
    if (bytes < 1024)
        snprintf(buf, sizeof buf, "%u B", (unsigned)bytes);
    else if (bytes < 1024ull * 1024)
        snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        snprintf(buf, sizeof buf, "%.1f MB", bytes / 1024.0 / 1024.0);
    else
        snprintf(buf, sizeof buf, "%.1f GB", bytes / 1024.0 / 1024.0 / 1024.0);
    return buf;
}

std::string StyleBlock()
{
    return "body{font-family:system-ui,'Segoe UI',sans-serif;background:#f1f5f9;color:#0f172a;"
           "margin:0;padding:32px}"
           "main{max-width:720px;margin:0 auto;background:#fff;border-radius:12px;"
           "box-shadow:0 4px 16px rgba(15,23,42,.08);overflow:hidden}"
           "h1{font-size:1rem;font-weight:600;padding:14px 20px;border-bottom:1px solid #e2e8f0;margin:0}"
           "table{width:100%;border-collapse:collapse;font-size:.925rem}"
           "td{padding:8px 20px;border-bottom:1px solid #f1f5f9}"
           "a{color:#2563eb;text-decoration:none}a:hover{text-decoration:underline}"
           ".size{color:#64748b;text-align:right;white-space:nowrap}";
}

std::string ErrorPage(int status, const std::string& detail)
{
    std::string title = std::to_string(status) + " " + StatusReason(status);
    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + HtmlEscape(title) +
           "</title><style>" + StyleBlock() +
           "p{padding:20px;line-height:1.6;color:#334155}</style></head><body><main>"
           "<h1>" + HtmlEscape(title) + "</h1><p>" + detail + "</p></main></body></html>";
}

bool ListDirectory(const std::string& fsDir, std::vector<DirEntry>& out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((fsDir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        unsigned long long size = isDir ? 0
            : (unsigned long long)fd.nFileSizeLow | ((unsigned long long)fd.nFileSizeHigh << 32);
        out.push_back({std::move(name), isDir, size});
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;  // directories first
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

std::string BuildListing(const std::string& fsDir, const std::string& urlDir)
{
    std::vector<DirEntry> entries;
    ListDirectory(fsDir, entries);

    std::string rows;
    if (urlDir != "/") {
        std::string parent = urlDir.substr(0, urlDir.size() - 1);
        size_t slash = parent.rfind('/');
        parent = parent.substr(0, slash == std::string::npos ? 0 : slash + 1);
        rows += "<tr><td><a href=\"" + UriEncode(parent) + "\">..</a></td><td class=\"size\"></td></tr>";
    }
    for (const auto& e : entries) {
        std::string href = UriEncode(urlDir == "/" ? "/" + e.name : urlDir + e.name) + (e.isDir ? "/" : "");
        rows += "<tr><td><a href=\"" + href + "\">" + HtmlEscape(e.name) + (e.isDir ? "/" : "") +
                "</a></td><td class=\"size\">" + (e.isDir ? "-" : HumanSize(e.size)) + "</td></tr>";
    }

    return "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of " +
           HtmlEscape(urlDir) + "</title><style>" + StyleBlock() + "</style></head><body><main>"
           "<h1>Index of " + HtmlEscape(urlDir) + "</h1><table>" + rows +
           "</table></main></body></html>";
}

std::string FsPathForRoot(const std::string& root, const std::string& relPath)
{
    // relPath is decoded, starts without '/', uses '/' separators, already
    // validated (no "..", no backslash, no colon).
    std::string p = root;
    while (!p.empty() && p.back() == '\\') p.pop_back();
    std::string rel = relPath;
    for (char& c : rel)
        if (c == '/') c = '\\';
    return p + "\\" + rel;
}

}  // namespace

void Router::SetRoot(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(mx_);
    root_ = dir;
}

const std::string Router::Root() const
{
    return root_;  // written once at start/before browse; read by GUI thread
}

size_t Router::RouteCount() const
{
    return routes_.size();  // written by GUI thread only
}

bool Router::LoadRoutes(const std::string& file, std::string& error)
{
    std::string text;
    if (!ReadFileBytes(file, 10 * 1024 * 1024, text)) {
        error = "cannot read " + file;
        return false;
    }

    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) {
        const char* ep = cJSON_GetErrorPtr();
        error = "parse error near \"" +
                (ep ? std::string(ep).substr(0, 32) : std::string("?")) + "\"";
        return false;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> guard(root, cJSON_Delete);

    if (!cJSON_IsArray(root)) {
        error = "top-level value must be an array of route objects";
        return false;
    }

    std::vector<MockRoute> parsed;
    std::string baseDir = file;
    size_t slash = baseDir.find_last_of("/\\");
    baseDir = slash == std::string::npos ? "." : baseDir.substr(0, slash);

    int index = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root)
    {
        ++index;
        if (!cJSON_IsObject(item)) {
            error = "item " + std::to_string(index) + " is not an object";
            return false;
        }

        MockRoute r;
        cJSON* jPath = cJSON_GetObjectItemCaseSensitive(item, "path");
        if (!cJSON_IsString(jPath) || jPath->valuestring[0] != '/') {
            error = "item " + std::to_string(index) + " needs a string \"path\" starting with '/'";
            return false;
        }
        r.path = jPath->valuestring;

        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "method"); cJSON_IsString(j))
            r.method = j->valuestring;

        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "status"); cJSON_IsNumber(j)) {
            int v = (int)j->valuedouble;
            if (v < 100 || v > 599) {
                error = "item " + std::to_string(index) + ": \"status\" must be 100-599";
                return false;
            }
            r.status = v;
        }

        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "delay_ms"); cJSON_IsNumber(j)) {
            int v = (int)j->valuedouble;
            if (v < 0 || v > 600000) {
                error = "item " + std::to_string(index) + ": \"delay_ms\" must be 0-600000";
                return false;
            }
            r.delayMs = v;
        }

        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "content_type"); cJSON_IsString(j))
            r.contentType = j->valuestring;

        // Body: "body" (raw string), "json" (serialized object) or "body_file"
        // (file path relative to routes.json).
        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "body"); cJSON_IsString(j))
            r.body = j->valuestring;
        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "json"); j && !cJSON_IsInvalid(j)) {
            char* printed = cJSON_PrintUnformatted(j);
            if (printed) {
                r.body = printed;
                cJSON_free(printed);
            }
        }
        if (cJSON* j = cJSON_GetObjectItemCaseSensitive(item, "body_file"); cJSON_IsString(j)) {
            std::string rel = j->valuestring;
            std::string full = rel.size() > 2 && rel[1] == ':' ? rel : baseDir + "\\" + rel;
            if (!ReadFileBytes(full, kMaxStaticFileBytes, r.body)) {
                error = "item " + std::to_string(index) + ": cannot read body_file \"" + rel + "\"";
                return false;
            }
        }

        parsed.push_back(std::move(r));
    }

    {
        std::lock_guard<std::mutex> lk(mx_);
        routes_ = std::move(parsed);
    }
    return true;
}

std::optional<MockRoute> Router::MatchMock(const HttpRequest& req)
{
    std::lock_guard<std::mutex> lk(mx_);
    for (const auto& r : routes_)
        if (r.method == req.method && r.path == req.path) return r;
    return std::nullopt;
}

HttpResponse Router::Handle(const HttpRequest& req)
{
    std::string root;
    {
        std::lock_guard<std::mutex> lk(mx_);
        root = root_;
    }

    if (auto route = MatchMock(req)) {
        if (route->delayMs > 0) Sleep((DWORD)route->delayMs);
        return HttpResponse{route->status, StatusReason(route->status),
                            route->contentType, route->body};
    }

    // --- Static files ---
    if (req.method != "GET" && req.method != "HEAD") {
        return HttpResponse{405, StatusReason(405), "text/html; charset=utf-8",
                            ErrorPage(405, "Only GET and HEAD are supported for static files.")};
    }

    std::string rel = req.path.substr(1);  // strip leading '/'

    if (rel.empty() || rel.back() == '/') {
        std::string indexFs = FsPathForRoot(root, rel.empty() ? std::string() : rel.substr(0, rel.size() - 1));
        indexFs += (indexFs.back() == '\\' ? "" : "\\") + std::string("index.html");
        DWORD attr = GetFileAttributesA(indexFs.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string body;
            if (ReadFileBytes(indexFs, kMaxStaticFileBytes, body))
                return HttpResponse{200, "OK", ContentTypeFor(indexFs), std::move(body)};
        }
        std::string dirFs = FsPathForRoot(root, rel.empty() ? std::string() : rel.substr(0, rel.size() - 1));
        attr = GetFileAttributesA(dirFs.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return HttpResponse{200, "OK", "text/html; charset=utf-8", BuildListing(dirFs, req.path)};
        return HttpResponse{404, StatusReason(404), "text/html; charset=utf-8",
                            ErrorPage(404, "The requested URL " + HtmlEscape(req.path) + " was not found.")};
    }

    std::string fsPath = FsPathForRoot(root, rel);
    DWORD attr = GetFileAttributesA(fsPath.c_str());

    if (attr == INVALID_FILE_ATTRIBUTES) {
        return HttpResponse{404, StatusReason(404), "text/html; charset=utf-8",
                            ErrorPage(404, "The requested URL " + HtmlEscape(req.path) + " was not found.")};
    }

    if (attr & FILE_ATTRIBUTE_DIRECTORY)
        return HttpResponse{200, "OK", "text/html; charset=utf-8", BuildListing(fsPath, req.path + "/")};

    std::string body;
    if (!ReadFileBytes(fsPath, kMaxStaticFileBytes, body)) {
        return HttpResponse{500, StatusReason(500), "text/html; charset=utf-8",
                            ErrorPage(500, "Failed to read \"" + HtmlEscape(rel) + "\".")};
    }
    return HttpResponse{200, "OK", ContentTypeFor(fsPath), std::move(body)};
}
