#include "gui.hpp"
#include "util.hpp"

#include <cJSON.h>

#include <iphlpapi.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <shlguid.h>
#include <shobjidl.h>
#include <windowsx.h>

namespace {

constexpr wchar_t kClassName[] = L"MiniCCServeWnd";

// All IPv4 addresses of up adapters (loopback excluded), for the address
// combo box and the shareable-URL status line. Empty when none are found.
std::vector<std::string> CollectLocalIps()
{
    std::vector<std::string> ips;
    ULONG size = 16 * 1024;
    std::string buf(size, '\0');
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                         GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addrs, &size) != NO_ERROR)
        return ips;

    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* unicast = a->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN] = {};
            auto* sa = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip)) {
                std::string s(ip);
                if (s != "127.0.0.1" && std::find(ips.begin(), ips.end(), s) == ips.end())
                    ips.push_back(s);
            }
        }
    }
    return ips;
}

// Layout metrics in 1/96th-dpi units; every pixel is derived via Scale().
constexpr int kMargin = 12;
constexpr int kGap = 8;
constexpr int kRowH = 30;
constexpr int kPortLabelW = 34;
constexpr int kAddrLabelW = 56;
constexpr int kPortEditW = 64;
constexpr int kBtnW = 88;
constexpr int kBrowseW = 88;
constexpr int kReloadW = 118;
constexpr int kStatusH = 22;
constexpr int kMinW = 640;
constexpr int kMinH = 490;

// --- Detail viewer: the double-click popup for one log row ---

constexpr wchar_t kViewerClass[] = L"MiniCCServeViewer";
constexpr int kViewerCopyBtn = 1001;
constexpr int kViewerDefW = 720;   // 1/96th-dpi units like the metrics above
constexpr int kViewerDefH = 520;
constexpr int kViewerToolH = 36;
constexpr int kViewerBtnW = 88;
constexpr int kViewerBtnH = 26;
constexpr int kViewerPad = 8;

HWND g_viewer = nullptr;  // single instance, reused across double-clicks

struct ViewerState {
    HWND edit = nullptr;
    HWND copyBtn = nullptr;
    HFONT monoFont = nullptr;
    HFONT uiFont = nullptr;
    int dpi = 96;
    int Scale(int v) const { return MulDiv(v, dpi, 96); }
};

void ViewerApplyFonts(ViewerState& st)
{
    if (st.monoFont) DeleteObject(st.monoFont);
    if (st.uiFont) DeleteObject(st.uiFont);
    st.monoFont = CreateFontW(-MulDiv(10, st.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    st.uiFont = CreateFontW(-MulDiv(9, st.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(st.edit, WM_SETFONT, (WPARAM)st.monoFont, TRUE);
    SendMessageW(st.copyBtn, WM_SETFONT, (WPARAM)st.uiFont, TRUE);
}

void ViewerLayout(HWND hwnd, ViewerState* st)
{
    if (!st) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int pad = st->Scale(kViewerPad);
    const int toolH = st->Scale(kViewerToolH);
    SetWindowPos(st->copyBtn, nullptr, w - pad - st->Scale(kViewerBtnW),
                 (toolH - st->Scale(kViewerBtnH)) / 2, st->Scale(kViewerBtnW),
                 st->Scale(kViewerBtnH), SWP_NOZORDER);
    SetWindowPos(st->edit, nullptr, pad, toolH, w - 2 * pad, h - toolH - pad, SWP_NOZORDER);
}

void CopyEditToClipboard(HWND edit)
{
    const int len = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(edit, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    if (!OpenClipboard(edit)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);  // ownership passes to the clipboard
    }
    CloseClipboard();
}

LRESULT CALLBACK ViewerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* st = reinterpret_cast<ViewerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = new ViewerState;
        st->dpi = GetDpiForWindow(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        st->copyBtn = CreateWindowExW(0, L"BUTTON", L"Copy all",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kViewerCopyBtn,
                                      cs->hInstance, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, hwnd, nullptr, cs->hInstance, nullptr);
        ViewerApplyFonts(*st);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kViewerCopyBtn && HIWORD(wp) == BN_CLICKED && st) {
            CopyEditToClipboard(st->edit);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {  // Esc, routed here by IsDialogMessage
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_SIZE:
        ViewerLayout(hwnd, st);
        return 0;
    case WM_DPICHANGED: {
        if (st) {
            st->dpi = HIWORD(wp);
            ViewerApplyFonts(*st);
        }
        auto* suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        if (st) {
            if (st->monoFont) DeleteObject(st->monoFont);
            if (st->uiFont) DeleteObject(st->uiFont);
            delete st;
        }
        g_viewer = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// One reusable viewer window; repeated double-clicks refresh its content.
void ShowLogDetail(HWND owner, int dpi, const std::wstring& title, const std::wstring& text)
{
    if (g_viewer) {
        SetWindowTextW(g_viewer, title.c_str());
        auto* st = reinterpret_cast<ViewerState*>(GetWindowLongPtrW(g_viewer, GWLP_USERDATA));
        if (st) {
            SetWindowTextW(st->edit, text.c_str());
            SendMessageW(st->edit, EM_SETSEL, 0, 0);
            SendMessageW(st->edit, EM_SCROLLCARET, 0, 0);
        }
        ShowWindow(g_viewer, SW_RESTORE);
        SetForegroundWindow(g_viewer);
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = &ViewerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kViewerClass;
    if (!RegisterClassW(&wc)) return;

    RECT rc = {0, 0, MulDiv(kViewerDefW, dpi, 96), MulDiv(kViewerDefH, dpi, 96)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_viewer = CreateWindowExW(0, kViewerClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                               owner, nullptr, wc.hInstance, nullptr);
    if (!g_viewer) return;
    auto* st = reinterpret_cast<ViewerState*>(GetWindowLongPtrW(g_viewer, GWLP_USERDATA));
    if (st) SetWindowTextW(st->edit, text.c_str());
    ShowWindow(g_viewer, SW_SHOW);
    SetForegroundWindow(g_viewer);
}

// Pretty-print via cJSON when the body parses; otherwise show it raw.
std::string PrettyIfJson(const std::string& body)
{
    if (body.empty()) return body;
    cJSON* parsed = cJSON_Parse(body.c_str());
    if (!parsed) return body;
    char* printed = cJSON_Print(parsed);
    cJSON_Delete(parsed);
    if (!printed) return body;
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

// One "── Label · content-type · N bytes ──" block of the viewer text.
std::string DetailSection(const char* label, const std::string& contentType,
                          size_t fullBytes, const std::string& body)
{
    std::string head = std::string("\xE2\x94\x80\xE2\x94\x80 ") + label + " \xC2\xB7 " +
                       (contentType.empty() ? "no Content-Type" : contentType) +
                       " \xC2\xB7 " + std::to_string(fullBytes) + " bytes \xE2\x94\x80\xE2\x94\x80\n";
    return head + (body.empty() ? "(empty)" : PrettyIfJson(body)) + "\n\n";
}

// The classic multiline EDIT control only breaks lines on CRLF; everything
// built here uses plain \n (cJSON_Print included), so expand before display.
std::wstring WithCrlf(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + s.size() / 8);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r'))
            out += L"\r\n";
        else
            out += s[i];
    }
    return out;
}

}  // namespace

bool MainWindow::Create(HINSTANCE instance, int nCmdShow, Settings& settings)
{
    settings_ = &settings;

    WNDCLASSW wc{};
    wc.style = 0;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassW(&wc)) return false;

    // Initial window client size at 96 dpi; adjusted on first WM_DPICHANGED.
    RECT rc = {0, 0, Scale(760), Scale(540)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME, FALSE);

    hwnd_ = CreateWindowExW(
        0, kClassName, L"MiniCCServe",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
}

int MainWindow::Run()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd_, &msg)) continue;  // TAB navigation
        if (g_viewer && (msg.hwnd == g_viewer || IsChild(g_viewer, msg.hwnd)) &&
            IsDialogMessageW(g_viewer, &msg))
            continue;  // TAB navigation + Esc-to-close in the viewer
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        hwnd_ = hwnd;
        SetDpi(GetDpiForWindow(hwnd));
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        CreateControls(cs->hInstance);
        ApplyFonts();
        Layout();

        SetWindowTextW(portEdit_, std::to_wstring(settings_->port).c_str());
        SetWindowTextW(addrCombo_, Utf8ToWide(settings_->address).c_str());
        SetWindowTextW(rootEdit_, Utf8ToWide(settings_->root).c_str());

        // Initial routes load; failures show up in the status bar.
        std::string err;
        if (router_.LoadRoutes(RoutesFilePath(), err))
            note_ = std::to_string(router_.RouteCount()) + " mock routes loaded";
        else
            note_ = "routes.json: " + err;
        UpdateStatus();
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IdStartBtn: if (HIWORD(wp) == BN_CLICKED) OnStart(); return 0;
        case IdStopBtn: if (HIWORD(wp) == BN_CLICKED) OnStop(); return 0;
        case IdReloadBtn: if (HIWORD(wp) == BN_CLICKED) OnReload(); return 0;
        case IdClearBtn: if (HIWORD(wp) == BN_CLICKED) OnClearLog(); return 0;
        case IdBrowseBtn: if (HIWORD(wp) == BN_CLICKED) OnBrowse(); return 0;
        }
        break;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom == IdLogList && nm->code == NM_DBLCLK) {
            auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lp);
            ShowEntryDetail(nmia->iItem);
            return 0;
        }
        break;
    }

    case kLogMsg: {
        auto* entry = reinterpret_cast<LogEntry*>(lp);
        AppendLog(*entry);
        delete entry;
        return 0;
    }

    case WM_SIZE:
        if (logList_) Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = Scale(kMinW);
        mmi->ptMinTrackSize.y = Scale(kMinH);
        return 0;
    }

    case WM_DPICHANGED: {
        SetDpi(HIWORD(wp));
        auto* suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyFonts();
        Layout();
        return 0;
    }

    case WM_DESTROY:
        server_.Stop();
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MainWindow::SetDpi(int dpi)
{
    if (dpi > 0) dpi_ = dpi;
}

void MainWindow::CreateControls(HINSTANCE instance)
{
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int id) {
        return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)id, instance, nullptr);
    };

    const DWORD editStyle = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    const DWORD btnStyle = WS_TABSTOP | BS_PUSHBUTTON;

    addrLabel_ = make(L"STATIC", L"Address:", SS_CENTERIMAGE, 0, IdAddrLabel);
    // Editable combo: 0.0.0.0 / 127.0.0.1 / every local adapter IP, plus
    // free-form input. Height includes the dropped-down list.
    addrCombo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_DISABLENOSCROLL,
                                 0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)IdAddrCombo, instance, nullptr);
    for (const wchar_t* preset : {L"0.0.0.0", L"127.0.0.1"})
        ComboBox_AddString(addrCombo_, preset);
    for (const std::string& ip : CollectLocalIps())
        ComboBox_AddString(addrCombo_, Utf8ToWide(ip).c_str());

    portLabel_ = make(L"STATIC", L"Port:", SS_CENTERIMAGE, 0, IdPortLabel);
    portEdit_ = make(L"EDIT", L"5555", editStyle | ES_NUMBER, WS_EX_CLIENTEDGE, IdPortEdit);

    rootLabel_ = make(L"STATIC", L"Root:", SS_CENTERIMAGE, 0, IdRootLabel);
    rootEdit_ = make(L"EDIT", L"", editStyle, WS_EX_CLIENTEDGE, IdRootEdit);
    browseBtn_ = make(L"BUTTON", L"Browse...", btnStyle, 0, IdBrowseBtn);

    startBtn_ = make(L"BUTTON", L"Start", btnStyle | BS_DEFPUSHBUTTON, 0, IdStartBtn);
    stopBtn_ = make(L"BUTTON", L"Stop", btnStyle, 0, IdStopBtn);
    reloadBtn_ = make(L"BUTTON", L"Reload routes", btnStyle, 0, IdReloadBtn);
    clearBtn_ = make(L"BUTTON", L"Clear log", btnStyle, 0, IdClearBtn);

    logList_ = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                               WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                                   LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                               0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)IdLogList, instance, nullptr);
    ListView_SetExtendedListViewStyle(logList_,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    const wchar_t* names[] = {L"Time", L"Method", L"Path", L"Status", L"Duration"};
    for (int i = 0; i < 5; ++i) {
        col.pszText = const_cast<LPWSTR>(names[i]);
        col.cx = 80;
        ListView_InsertColumn(logList_, i, &col);
    }

    statusText_ = make(L"STATIC", L"Stopped", SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, IdStatusText);

    EnableWindow(stopBtn_, FALSE);
}

void MainWindow::ApplyFonts()
{
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    for (HWND h : {addrLabel_, addrCombo_, portLabel_, portEdit_, rootLabel_, rootEdit_,
                   browseBtn_, startBtn_, stopBtn_, reloadBtn_, clearBtn_, logList_, statusText_})
        SendMessageW(h, WM_SETFONT, (WPARAM)font_, TRUE);
}

// All rows share one height, all gaps one width, all edges one margin — the
// alignment contract of this UI.
void MainWindow::Layout()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int cw = rc.right;
    const int ch = rc.bottom;

    const int M = Scale(kMargin), G = Scale(kGap), H = Scale(kRowH);
    const int statusH = Scale(kStatusH);
    const int textOff = Scale(9);  // optical centering for vertically stacked rows

    // Row 1: Address combo (fills) + Port edit (anchored at the right edge)
    int y = M;
    SetWindowPos(addrLabel_, nullptr, M, y, Scale(kAddrLabelW), H, SWP_NOZORDER);
    const int portX = cw - M - Scale(kPortEditW);
    SetWindowPos(addrCombo_, nullptr, M + Scale(kAddrLabelW), y,
                 portX - Scale(kPortLabelW) - M - Scale(kAddrLabelW) - G * 2, H + Scale(100), SWP_NOZORDER);
    SetWindowPos(portLabel_, nullptr, portX - Scale(kPortLabelW), y, Scale(kPortLabelW), H, SWP_NOZORDER);
    SetWindowPos(portEdit_, nullptr, portX, y, Scale(kPortEditW), H, SWP_NOZORDER);

    // Row 2: Root edit (aligned with the Address combo) + Browse
    y += H + G;
    SetWindowPos(rootLabel_, nullptr, M, y, Scale(kAddrLabelW), H, SWP_NOZORDER);
    const int rootX = M + Scale(kAddrLabelW);
    const int browseX = cw - M - Scale(kBrowseW);
    SetWindowPos(rootEdit_, nullptr, rootX, y, browseX - rootX - G, H, SWP_NOZORDER);
    SetWindowPos(browseBtn_, nullptr, browseX, y, Scale(kBrowseW), H, SWP_NOZORDER);

    // Row 3: Start + Stop + Reload + Clear
    y += H + G;
    int bx = M;
    SetWindowPos(startBtn_, nullptr, bx, y, Scale(kBtnW), H, SWP_NOZORDER);
    bx += Scale(kBtnW) + G;
    SetWindowPos(stopBtn_, nullptr, bx, y, Scale(kBtnW), H, SWP_NOZORDER);
    bx += Scale(kBtnW) + G;
    SetWindowPos(reloadBtn_, nullptr, bx, y, Scale(kReloadW), H, SWP_NOZORDER);
    bx += Scale(kReloadW) + G;
    SetWindowPos(clearBtn_, nullptr, bx, y, Scale(kBtnW), H, SWP_NOZORDER);

    // Log list fills the middle, status line sits at the bottom
    y += H + G;
    const int statusY = ch - M - statusH;
    SetWindowPos(logList_, nullptr, M, y, cw - 2 * M, statusY - y - G, SWP_NOZORDER);
    SetWindowPos(statusText_, nullptr, M, statusY + (statusH - Scale(13)) / 2,
                 cw - 2 * M, Scale(13) + textOff / 3, SWP_NOZORDER);

    // Column widths: fixed first two and last two, Path takes the rest.
    const int timeW = Scale(76), methodW = Scale(58), statusW = Scale(56), durW = Scale(74);
    int listW = cw - 2 * M - GetSystemMetrics(SM_CXVSCROLL) - Scale(4);
    int pathW = listW - timeW - methodW - statusW - durW - Scale(24);
    if (pathW < Scale(120)) pathW = Scale(120);
    int widths[5] = {timeW, methodW, pathW, statusW, durW};
    for (int i = 0; i < 5; ++i)
        ListView_SetColumnWidth(logList_, i, widths[i]);
}

void MainWindow::AppendLog(const LogEntry& entry)
{
    int count = ListView_GetItemCount(logList_);
    if (count >= kMaxLogRows) {
        ListView_DeleteItem(logList_, 0);
        --count;
        if (!details_.empty()) details_.erase(details_.begin());
    }

    const std::wstring time = Utf8ToWide(entry.time);
    const std::wstring method = Utf8ToWide(entry.method);
    const std::wstring path = Utf8ToWide(entry.path);
    const std::wstring status = std::to_wstring(entry.status);
    const std::wstring dur = std::to_wstring(entry.durationMs) + L" ms";

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = count;
    lvi.pszText = const_cast<LPWSTR>(time.c_str());
    int idx = ListView_InsertItem(logList_, &lvi);
    if (idx < 0) return;
    ListView_SetItemText(logList_, idx, 1, const_cast<LPWSTR>(method.c_str()));
    ListView_SetItemText(logList_, idx, 2, const_cast<LPWSTR>(path.c_str()));
    ListView_SetItemText(logList_, idx, 3, const_cast<LPWSTR>(status.c_str()));
    ListView_SetItemText(logList_, idx, 4, const_cast<LPWSTR>(dur.c_str()));
    ListView_EnsureVisible(logList_, idx, FALSE);

    details_.push_back(entry);  // row i of the list mirrors details_[i]
    ++requests_;
    UpdateStatus();
}

void MainWindow::ShowEntryDetail(int index)
{
    if (index < 0 || index >= (int)details_.size()) return;
    const LogEntry& e = details_[index];

    std::string title = e.method + " " + e.path + (e.query.empty() ? "" : "?" + e.query) +
                        " \xE2\x80\x94 " + std::to_string(e.status) + " " + StatusReason(e.status);
    std::string text = DetailSection("Request", e.reqContentType, e.reqBodySize, e.reqBody) +
                       DetailSection("Response", e.respContentType, e.respBodySize, e.respBody);
    ShowLogDetail(hwnd_, dpi_, Utf8ToWide(title), WithCrlf(Utf8ToWide(text)));
}

void MainWindow::UpdateStatus()
{
    std::string s;
    if (server_.Running()) {
        // 0.0.0.0 means "every interface": advertise the LAN address other
        // machines can actually reach. A specific address is shown as-is.
        std::string shown = settings_->address;
        if (shown == "0.0.0.0") {
            auto ips = CollectLocalIps();
            if (!ips.empty()) shown = ips.front();
        }
        s = "Running  \xC2\xB7  http://" + shown + ":" + std::to_string(settings_->port) +
            "  \xC2\xB7  " + std::to_string(requests_) + " requests";
    } else {
        s = "Stopped  \xC2\xB7  " + note_;
    }
    SetWindowTextW(statusText_, Utf8ToWide(s).c_str());
}

void MainWindow::SetControlsRunning(bool running)
{
    EnableWindow(portEdit_, !running);
    EnableWindow(addrCombo_, !running);
    EnableWindow(rootEdit_, !running);
    EnableWindow(browseBtn_, !running);
    EnableWindow(startBtn_, !running);
    EnableWindow(stopBtn_, running);
    if (running) SetFocus(stopBtn_);
    else SetFocus(startBtn_);
}

std::string MainWindow::RootFromEdit() const
{
    int len = GetWindowTextLengthW(rootEdit_);
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(rootEdit_, w.data(), len + 1);
    w.resize(static_cast<size_t>(len));  // drop the NUL terminator
    std::string root = Trim(WideToUtf8(w));
    if (root.empty()) root = ExeDir() + "\\www";
    return root;
}

std::string MainWindow::AddressFromEdit() const
{
    int len = GetWindowTextLengthW(addrCombo_);
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(addrCombo_, w.data(), len + 1);
    w.resize(static_cast<size_t>(len));
    std::string address = Trim(WideToUtf8(w));
    return address.empty() ? std::string("0.0.0.0") : address;
}

int MainWindow::PortFromEdit(bool& ok) const
{
    BOOL translated = FALSE;
    UINT port = GetDlgItemInt(hwnd_, IdPortEdit, &translated, FALSE);
    ok = translated && port >= 1 && port <= 65535;
    return ok ? (int)port : -1;
}

void MainWindow::OnStart()
{
    bool ok = false;
    int port = PortFromEdit(ok);
    if (!ok) {
        MessageBoxW(hwnd_, L"Port must be a number between 1 and 65535.",
                    L"MiniCCServe", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string address = AddressFromEdit();
    in_addr probe{};
    if (inet_pton(AF_INET, address.c_str(), &probe) != 1) {
        std::wstring msg = Utf8ToWide("\"" + address + "\" is not a valid IPv4 address.");
        MessageBoxW(hwnd_, msg.c_str(), L"MiniCCServe", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string root = RootFromEdit();
    DWORD attr = GetFileAttributesA(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring msg = L"The root directory does not exist:\n" + Utf8ToWide(root);
        MessageBoxW(hwnd_, msg.c_str(), L"MiniCCServe", MB_OK | MB_ICONWARNING);
        return;
    }

    settings_->port = port;
    settings_->address = address;
    settings_->root = root;
    router_.SetRoot(root);

    server_.SetLogFn([this](LogEntry e) {
        PostMessageW(hwnd_, kLogMsg, 0, (LPARAM) new LogEntry(std::move(e)));
    });

    std::string error;
    if (!server_.Start(port, address, router_, error)) {
        std::wstring msg = Utf8ToWide("Cannot start the server.\n\n" + error);
        MessageBoxW(hwnd_, msg.c_str(), L"MiniCCServe", MB_OK | MB_ICONERROR);
        return;
    }

    SetControlsRunning(true);
    UpdateStatus();
}

void MainWindow::OnStop()
{
    server_.Stop();
    SetControlsRunning(false);
    UpdateStatus();
}

void MainWindow::OnReload()
{
    std::string error;
    if (router_.LoadRoutes(RoutesFilePath(), error))
        note_ = std::to_string(router_.RouteCount()) + " mock routes loaded";
    else
        note_ = "routes.json: " + error;
    UpdateStatus();
}

void MainWindow::OnClearLog()
{
    ListView_DeleteAllItems(logList_);
    details_.clear();  // the request counter stays cumulative by design
}

void MainWindow::OnBrowse()
{
    std::string folder;
    if (PickFolder(folder)) {
        SetWindowTextW(rootEdit_, Utf8ToWide(folder).c_str());
        settings_->root = folder;
    }
}

bool MainWindow::PickFolder(std::string& out)
{
    bool picked = false;
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return false;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    std::wstring current = Utf8ToWide(RootFromEdit());
    if (!current.empty() && GetFileAttributesW(current.c_str()) != INVALID_FILE_ATTRIBUTES) {
        IShellItem* startAt = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&startAt)))) {
            dlg->SetDefaultFolder(startAt);
            startAt->Release();
        }
    }

    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dlg->GetResult(&result))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = WideToUtf8(path);
                CoTaskMemFree(path);
                picked = true;
            }
            result->Release();
        }
    }
    dlg->Release();
    return picked;
}
