#include "config.hpp"

#include "util.hpp"

#include <windows.h>

namespace {

const char* kIniSection = "server";
const char* kIniName = "settings.ini";

std::string IniPath()
{
    return ExeDir() + "\\" + kIniName;
}

std::string DefaultRoot()
{
    return ExeDir() + "\\www";
}

const char* kExampleRoutes =
    "[\n"
    "  {\n"
    "    \"comment\": \"Return a fixed JSON payload after an artificial delay.\",\n"
    "    \"method\": \"GET\",\n"
    "    \"path\": \"/api/hello\",\n"
    "    \"status\": 200,\n"
    "    \"delay_ms\": 500,\n"
    "    \"json\": { \"message\": \"Hello from a MiniCCServe mock route\" }\n"
    "  },\n"
    "  {\n"
    "    \"comment\": \"Echo service: send any POST body, get it back in JSON.\",\n"
    "    \"method\": \"POST\",\n"
    "    \"path\": \"/api/echo\",\n"
    "    \"status\": 200,\n"
    "    \"json\": { \"echo\": true, \"note\": \"edit routes.json to change this\" }\n"
    "  },\n"
    "  {\n"
    "    \"comment\": \"Error-branch testing: always answers 503.\",\n"
    "    \"method\": \"GET\",\n"
    "    \"path\": \"/api/unavailable\",\n"
    "    \"status\": 503,\n"
    "    \"json\": { \"error\": \"service unavailable\" }\n"
    "  }\n"
    "]\n";

}  // namespace

std::string ExeDir()
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, n);
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

Settings LoadSettings()
{
    Settings s;
    const std::string ini = IniPath();

    int port = GetPrivateProfileIntA(kIniSection, "port", 5555, ini.c_str());
    if (port < 1 || port > 65535) port = 5555;
    s.port = port;

    char address[64] = "0.0.0.0";
    GetPrivateProfileStringA(kIniSection, "address", "0.0.0.0", address, sizeof address, ini.c_str());
    s.address = address;

    char root[MAX_PATH] = {};
    GetPrivateProfileStringA(kIniSection, "root", "", root, MAX_PATH, ini.c_str());
    s.root = root[0] ? std::string(root) : DefaultRoot();
    return s;
}

void SaveSettings(const Settings& settings)
{
    WritePrivateProfileStringA(kIniSection, "port", std::to_string(settings.port).c_str(),
                               IniPath().c_str());
    WritePrivateProfileStringA(kIniSection, "address", settings.address.c_str(), IniPath().c_str());
    WritePrivateProfileStringA(kIniSection, "root", settings.root.c_str(), IniPath().c_str());
}

std::string RoutesFilePath()
{
    return ExeDir() + "\\routes.json";
}

void EnsureExampleRoutesFile()
{
    const std::string path = RoutesFilePath();
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, kExampleRoutes, (DWORD)strlen(kExampleRoutes), &written, nullptr);
    CloseHandle(h);
}
