#pragma once

// Main window: controls, DPI-aware layout, request log, start/stop wiring.

#include "common.hpp"
#include "config.hpp"
#include "http.hpp"
#include "router.hpp"

#include <string>
#include <vector>

class MainWindow {
public:
    bool Create(HINSTANCE instance, int nCmdShow, Settings& settings);
    int Run();

private:
    // Control identifiers (creation order == TAB order). IdStartBtn/IdStopBtn
    // keep their values — scripts/smoke-test.py drives them via WM_COMMAND.
    enum : int {
        IdAddrLabel = 101,
        IdAddrCombo,
        IdPortLabel,
        IdPortEdit,
        IdRootLabel,
        IdRootEdit,
        IdBrowseBtn,
        IdStartBtn,   // 108
        IdStopBtn,    // 109
        IdReloadBtn,
        IdClearBtn,
        IdLogList,
        IdStatusText
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls(HINSTANCE instance);
    void ApplyFonts();
    void Layout();
    void SetDpi(int dpi);
    int Scale(int v) const { return MulDiv(v, dpi_, 96); }

    void AppendLog(const LogEntry& entry);
    void ShowEntryDetail(int index);
    void UpdateStatus();
    void SetControlsRunning(bool running);

    void OnStart();
    void OnStop();
    void OnReload();
    void OnClearLog();
    void OnBrowse();
    bool PickFolder(std::string& out);

    std::string RootFromEdit() const;
    std::string AddressFromEdit() const;
    int PortFromEdit(bool& ok) const;

    static constexpr int kLogMsg = WM_APP + 1;   // LPARAM -> LogEntry* (ownership passes)
    static constexpr int kMaxLogRows = 2000;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;

    HWND addrLabel_ = nullptr, addrCombo_ = nullptr;
    HWND portLabel_ = nullptr, portEdit_ = nullptr;
    HWND rootLabel_ = nullptr, rootEdit_ = nullptr, browseBtn_ = nullptr;
    HWND startBtn_ = nullptr, stopBtn_ = nullptr, reloadBtn_ = nullptr, clearBtn_ = nullptr;
    HWND logList_ = nullptr, statusText_ = nullptr;

    // Row i of logList_ mirrors details_[i]; trimmed at the front like the rows.
    std::vector<LogEntry> details_;

    Settings* settings_ = nullptr;
    HttpServer server_;
    Router router_;
    long long requests_ = 0;
    std::string note_;  // transient hint shown in the status bar
};
