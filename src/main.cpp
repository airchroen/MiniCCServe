// MiniCCServe — a tiny Windows desktop server for debugging service projects:
// static file serving plus user-defined mock routes from routes.json.

#include "common.hpp"
#include "config.hpp"
#include "gui.hpp"

#include <objbase.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prevInstance, LPSTR cmdLine, int nCmdShow)
{
    (void)prevInstance;
    (void)cmdLine;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, L"Winsock 2.2 initialization failed.", L"MiniCCServe",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    // Bootstrap: default www folder next to the exe + example routes.json.
    Settings settings = LoadSettings();
    CreateDirectoryA(settings.root.c_str(), nullptr);
    EnsureExampleRoutesFile();

    MainWindow window;
    if (!window.Create(instance, nCmdShow, settings)) {
        MessageBoxW(nullptr, L"Window creation failed.", L"MiniCCServe", MB_OK | MB_ICONERROR);
        WSACleanup();
        CoUninitialize();
        return 1;
    }

    int rc = window.Run();

    SaveSettings(settings);
    WSACleanup();
    CoUninitialize();
    return rc;
}
