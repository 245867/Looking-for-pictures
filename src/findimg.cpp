#define UNICODE
#define _UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <string>
#include <regex>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <uxtheme.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// 私密配置：密钥与默认目标窗口
//
// 真实密钥不随仓库分发。请复制 src/secrets.example.h 为 src/secrets.h，
// 填入自己的 QQ 邮箱 SMTP 授权码与大漠插件注册码即可。
// secrets.h 已在 .gitignore 中，不会被提交；不提供该文件时以下默认值生效
// （邮件通知不可用、大漠未注册，其余功能正常）。
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#ifndef FINDIMG_SMTP_USER
#  define FINDIMG_SMTP_USER  L""   // 发件邮箱，如 L"you@qq.com"
#endif
#ifndef FINDIMG_SMTP_PASS
#  define FINDIMG_SMTP_PASS  L""   // QQ 邮箱 SMTP 授权码（非登录密码）
#endif
#ifndef FINDIMG_MAIL_TO
#  define FINDIMG_MAIL_TO    L""   // 收件邮箱
#endif
#ifndef FINDIMG_DM_CODE
#  define FINDIMG_DM_CODE    L""   // 大漠插件注册码
#endif
// 默认目标窗口类名/标题。标题为 nullptr 表示不按标题匹配，
// 由用户在界面上点“拖住寻找”自行选择目标窗口。
#ifndef FINDIMG_TARGET_CLASS
#  define FINDIMG_TARGET_CLASS L"UnrealWindow"
#endif
#ifndef FINDIMG_TARGET_TITLE
#  define FINDIMG_TARGET_TITLE nullptr
#endif

static const int ID_THRESHOLD = 101, ID_INTERVAL = 102, ID_COOLDOWN = 103, ID_AWAY = 104, ID_HWND = 105, ID_FORCE_START = 106, ID_FORCE_END = 107;
static const int ID_CLICK = 111, ID_FLASH = 113, ID_EMAIL = 114;
static const int ID_SQUEEZE = 115;
static const int ID_REFRESH = 121, ID_START = 131, ID_TEST = 132, ID_OPEN = 133, ID_CLEAR = 134, ID_KEYSIM = 135;
static const int ID_PICK = 122, ID_SCHEME_BASE = 2000, ID_SCHEME_TEST_BASE = 2100;
static const int ID_LABEL_THRESHOLD = 141, ID_LABEL_INTERVAL = 142, ID_LABEL_COOLDOWN = 143, ID_LABEL_AWAY = 144, ID_LABEL_HWND = 145, ID_LABEL_SCHEME = 146, ID_LABEL_FORCE = 147;
static const UINT WM_LOG = WM_APP + 1, WM_SCAN_DONE = WM_APP + 2, WM_WORKER_DONE = WM_APP + 3, WM_KEYSIM_DONE = WM_APP + 4, WM_JADE_STOP = WM_APP + 5, WM_JADE_FLASH = WM_APP + 6;
static const int MIN_COOLDOWN = 15;
static const int DEFAULT_AWAY_MINUTES = 60;
static const wchar_t* SETTINGS_FILE = L"findimg_settings.json";
static const wchar_t* SMTP_USER = FINDIMG_SMTP_USER;
static const wchar_t* SMTP_PASS = FINDIMG_SMTP_PASS;
static const wchar_t* MAIL_TO = FINDIMG_MAIL_TO;

struct Template {
    std::wstring group, name;
    int w = 0, h = 0;
    std::vector<uint8_t> gray, mask;
    std::vector<int> valid, anchors;
};
struct Match { bool ok = false; double sim = -1; int x = 0, y = 0; const Template* t = nullptr; };
struct Screen { int left = 0, top = 0, w = 0, h = 0; std::vector<uint8_t> gray; };

static HWND g_hwnd = nullptr;
static HINSTANCE g_inst = nullptr;
static ULONG_PTR g_gdiplus = 0;
static std::atomic<bool> g_running(false), g_stop(false), g_testing(false);
static std::atomic<bool> g_squeeze_triggered(false);
static std::thread g_worker;
static std::thread g_key_worker;
static std::atomic<bool> g_key_running(false), g_key_stop(false);
static std::mutex g_mutex;
static std::map<std::wstring, bool> g_groups;
static std::map<std::wstring, HWND> g_group_controls;
static std::map<int, HWND> g_scheme_controls;
static std::map<int, HWND> g_scheme_test_controls;
static HFONT g_ui_font = nullptr;
static HBRUSH g_bg_brush = CreateSolidBrush(RGB(8,8,14)), g_panel_brush = CreateSolidBrush(RGB(14,14,24)), g_edit_brush = CreateSolidBrush(RGB(20,22,35));
static std::wstring g_base;
static bool g_settings_loaded = false;
static bool g_startup_complete = false;
// 0 means no window-capture scheme selected: use the legacy full-screen scan.
static int g_scheme = 0;
static std::atomic<int> g_test_scheme(-1);
static HWND g_target = nullptr;
static LRESULT CALLBACK TransparentCheckProc(HWND, UINT, WPARAM, LPARAM);
static void EnsureForceControls();
static std::mutex g_log_mutex;
static std::string WideToUtf8(const std::wstring& s);
static std::wstring HwndText(HWND h);
static void RegisterDmAtStartup();

static void AppendDiagnostic(const std::wstring& text) {
    if (g_base.empty()) return;
    SYSTEMTIME st{}; GetLocalTime(&st);
    std::wstring line = L"[" + std::to_wstring(st.wYear) + L"-" + (st.wMonth < 10 ? L"0" : L"") + std::to_wstring(st.wMonth) + L"-" + (st.wDay < 10 ? L"0" : L"") + std::to_wstring(st.wDay) + L" " + (st.wHour < 10 ? L"0" : L"") + std::to_wstring(st.wHour) + L":" + (st.wMinute < 10 ? L"0" : L"") + std::to_wstring(st.wMinute) + L":" + (st.wSecond < 10 ? L"0" : L"") + std::to_wstring(st.wSecond) + L"] " + text + L"\n";
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream f(fs::path(g_base) / L"findimg.log", std::ios::binary | std::ios::app);
    std::string utf8 = WideToUtf8(line); f.write(utf8.data(), (std::streamsize)utf8.size()); f.flush();
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) AppendDiagnostic(L"未处理异常，代码 0x" + HwndText((HWND)(UINT_PTR)ep->ExceptionRecord->ExceptionCode) + L"，地址 0x" + HwndText((HWND)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress));
    else AppendDiagnostic(L"未处理异常");
    return EXCEPTION_EXECUTE_HANDLER;
}
static void TerminateHandler() { AppendDiagnostic(L"std::terminate 被调用"); abort(); }
struct CrashHooks { CrashHooks() { SetUnhandledExceptionFilter(CrashFilter); std::set_terminate(TerminateHandler); } };
static CrashHooks g_crash_hooks;

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
// JadeView IPC callbacks and detector workers do not share a Win32 message loop.
// Keep an in-process snapshot so scanning never synchronously queries hidden controls.
#ifdef JADE_FRONTEND
static std::mutex g_jade_config_mutex;
static std::map<int, std::wstring> g_jade_text;
static std::map<int, bool> g_jade_checks;
static bool g_jade_config_ready = false;
static void SetJadeText(int id, const std::wstring& value) { std::lock_guard<std::mutex> lock(g_jade_config_mutex); g_jade_text[id] = value; }
static void SetJadeChecked(int id, bool value) { std::lock_guard<std::mutex> lock(g_jade_config_mutex); g_jade_checks[id] = value; }
static void InitJadeConfigSnapshot() {
    const int texts[] = { ID_THRESHOLD, ID_INTERVAL, ID_COOLDOWN, ID_AWAY, ID_HWND, ID_FORCE_START, ID_FORCE_END };
    std::lock_guard<std::mutex> lock(g_jade_config_mutex);
    for (int id : texts) { wchar_t b[128]{}; GetWindowTextW(GetDlgItem(g_hwnd, id), b, 128); g_jade_text[id] = b; }
    for (int id : { ID_CLICK, ID_FLASH, ID_EMAIL, ID_SQUEEZE }) if (!g_jade_checks.count(id)) g_jade_checks[id] = SendDlgItemMessageW(g_hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_jade_config_ready = true;
}
static std::wstring ReadCtrl(int id) { std::lock_guard<std::mutex> lock(g_jade_config_mutex); if (g_jade_config_ready) { auto i = g_jade_text.find(id); return i == g_jade_text.end() ? L"" : i->second; } wchar_t b[128]{}; GetWindowTextW(GetDlgItem(g_hwnd, id), b, 128); return b; }
static bool Checked(int id) { std::lock_guard<std::mutex> lock(g_jade_config_mutex); return g_jade_checks[id]; }
#else
static std::wstring ReadCtrl(int id) { wchar_t b[128]{}; GetWindowTextW(GetDlgItem(g_hwnd, id), b, 128); return b; }
static bool Checked(int id) { return SendDlgItemMessageW(g_hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
#endif
static double Number(int id, double d) { try { return std::stod(ReadCtrl(id)); } catch (...) { return d; } }
static void SetChecked(int id, bool v) {
#ifdef JADE_FRONTEND
    SetJadeChecked(id, v);
#else
    SendDlgItemMessageW(g_hwnd, id, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
#endif
}
static std::wstring HwndText(HWND h) { std::wstringstream s; s << L"0x" << std::hex << std::uppercase << (UINT_PTR)h; return s.str(); }
static HWND ParseHwnd(const std::wstring& text) {
    try { size_t used = 0; UINT_PTR v = std::stoull(text, &used, 0); return (HWND)v; }
    catch (...) { return nullptr; }
}
static int ActiveScheme() { int n = g_test_scheme.load(); return n >= 0 ? n : g_scheme; }

static std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), {});
}
static std::string JsonString(const std::string& text, const char* key, const std::string& d) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = text.find(needle); if (p == std::string::npos) return d;
    p = text.find(':', p + needle.size()); if (p == std::string::npos) return d;
    p = text.find('"', p + 1); if (p == std::string::npos) return d;
    size_t e = text.find('"', p + 1); return e == std::string::npos ? d : text.substr(p + 1, e - p - 1);
}
static bool JsonBool(const std::string& text, const char* key, bool d) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = text.find(needle); if (p == std::string::npos) return d;
    p = text.find(':', p + needle.size()); if (p == std::string::npos) return d;
    p = text.find_first_not_of(" \t\r\n", p + 1); if (p == std::string::npos) return d;
    if (text.compare(p, 4, "true") == 0) return true;
    if (text.compare(p, 5, "false") == 0) return false;
    return d;
}
static void LoadSettings() {
    EnsureForceControls();
    std::string text = ReadText(fs::path(g_base) / SETTINGS_FILE);
    for (auto& [k, v] : g_groups) v = true;
    auto pos = text.find("\"group_selection\"");
    if (pos != std::string::npos) {
        auto end = text.find('}', pos); if (end == std::string::npos) end = text.size();
        std::string part = text.substr(pos, end - pos);
        std::regex r("\"([^\"]+)\"\\s*:\\s*(true|false)");
        for (std::sregex_iterator i(part.begin(), part.end(), r), e; i != e; ++i) {
            std::wstring k = Utf8ToWide((*i)[1].str());
            if (g_groups.count(k)) g_groups[k] = (*i)[2].str() == "true";
        }
    }
    SetWindowTextW(GetDlgItem(g_hwnd, ID_THRESHOLD), Utf8ToWide(JsonString(text, "threshold", "0.95")).c_str());
    SetWindowTextW(GetDlgItem(g_hwnd, ID_INTERVAL), Utf8ToWide(JsonString(text, "interval", "1.0")).c_str());
    SetWindowTextW(GetDlgItem(g_hwnd, ID_COOLDOWN), Utf8ToWide(JsonString(text, "cooldown", "15")).c_str());
    SetWindowTextW(GetDlgItem(g_hwnd, ID_AWAY), Utf8ToWide(JsonString(text, "away_minutes", "60")).c_str());
    SetWindowTextW(GetDlgItem(g_hwnd, ID_FORCE_START), Utf8ToWide(JsonString(text, "force_start", "")).c_str());
    SetWindowTextW(GetDlgItem(g_hwnd, ID_FORCE_END), Utf8ToWide(JsonString(text, "force_end", "")).c_str());
    SetWindowTextW(GetDlgItem(g_hwnd, ID_HWND), Utf8ToWide(JsonString(text, "target_hwnd", "")).c_str());
    try { g_scheme = (std::max)(0, (std::min)(10, std::stoi(JsonString(text, "scheme", "0")))); } catch (...) { g_scheme = 0; }
    g_target = ParseHwnd(ReadCtrl(ID_HWND));
    SetChecked(ID_CLICK, JsonBool(text, "click", true));
    SetChecked(ID_FLASH, JsonBool(text, "flash", true)); SetChecked(ID_EMAIL, JsonBool(text, "email", false));
    SetChecked(ID_SQUEEZE, JsonBool(text, "squeeze", false));
    for (auto& [g, h] : g_group_controls) { SendMessageW(h, BM_SETCHECK, g_groups[g] ? BST_CHECKED : BST_UNCHECKED, 0); }
    g_settings_loaded = true;
}
static void SaveSettings() {
    if (!g_startup_complete) return;
    std::ostringstream o;
    o << "{\n  \"group_selection\": {\n";
    bool first = true; for (const auto& [g, v] : g_groups) { if (!first) o << ",\n"; first = false; o << "    \"" << WideToUtf8(g) << "\": " << (v ? "true" : "false"); }
    o << "\n  },\n  \"threshold\": \"" << WideToUtf8(ReadCtrl(ID_THRESHOLD)) << "\",\n"
      << "  \"interval\": \"" << WideToUtf8(ReadCtrl(ID_INTERVAL)) << "\",\n"
      << "  \"cooldown\": \"" << WideToUtf8(ReadCtrl(ID_COOLDOWN)) << "\",\n"
      << "  \"away_minutes\": \"" << WideToUtf8(ReadCtrl(ID_AWAY)) << "\",\n"
      << "  \"force_start\": \"" << WideToUtf8(ReadCtrl(ID_FORCE_START)) << "\",\n"
      << "  \"force_end\": \"" << WideToUtf8(ReadCtrl(ID_FORCE_END)) << "\",\n"
      << "  \"target_hwnd\": \"" << WideToUtf8(ReadCtrl(ID_HWND)) << "\",\n"
      << "  \"scheme\": \"" << g_scheme << "\",\n"
      << "  \"click\": " << (Checked(ID_CLICK) ? "true" : "false") << ",\n"
      << "  \"flash\": " << (Checked(ID_FLASH) ? "true" : "false") << ",\n"
      << "  \"email\": " << (Checked(ID_EMAIL) ? "true" : "false") << ",\n"
      << "  \"squeeze\": " << (Checked(ID_SQUEEZE) ? "true" : "false") << "\n}\n";
    std::ofstream f(fs::path(g_base) / SETTINGS_FILE, std::ios::binary); f << o.str();
}
static void Log(const std::wstring& s) { AppendDiagnostic(s); if (g_hwnd) PostMessageW(g_hwnd, WM_LOG, 0, (LPARAM)new std::wstring(s)); }
static std::wstring GroupKey(const std::wstring& n) {
    size_t dot = n.find_last_of(L'.'); std::wstring s = dot == std::wstring::npos ? n : n.substr(0, dot);
    while (!s.empty() && s.back() >= L'0' && s.back() <= L'9') s.pop_back(); return s.empty() ? n : s;
}
static std::vector<std::wstring> ImageFiles() {
    std::vector<std::wstring> out; fs::path d = fs::path(g_base) / L"img";
    if (!fs::exists(d)) return out;
    for (auto& e : fs::directory_iterator(d)) if (e.is_regular_file()) {
        auto x = e.path().extension().wstring(); for (auto& c : x) c = (wchar_t)towlower(c);
        if (x == L".bmp" || x == L".png" || x == L".jpg" || x == L".jpeg" || x == L".webp") out.push_back(e.path().filename().wstring());
    }
    std::sort(out.begin(), out.end()); return out;
}
static bool IsSqueezeTemplate(const std::wstring& name);
static void RefreshGroups(bool log = false) {
    for (auto& [g, h] : g_group_controls) DestroyWindow(h); g_group_controls.clear();
    std::map<std::wstring, std::vector<std::wstring>> groups;
    for (auto& n : ImageFiles()) if (!IsSqueezeTemplate(n)) groups[GroupKey(n)].push_back(n);
    for (auto& [g, files] : groups) if (!g_groups.count(g)) g_groups[g] = true;
    int i = 0; for (auto& [g, files] : groups) {
        std::wstring t = g + L" (" + std::to_wstring(files.size()) + L"张)";
        HWND h = CreateWindowW(L"BUTTON", t.c_str(), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_FLAT,
            0, 0, 0, 0, g_hwnd, (HMENU)(3000 + i), g_inst, nullptr);
        if (g_ui_font) SendMessageW(h, WM_SETFONT, (WPARAM)g_ui_font, TRUE); else SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE); SendMessageW(h, BM_SETCHECK, g_groups[g] ? BST_CHECKED : BST_UNCHECKED, 0);
        g_group_controls[g] = h; ++i;
    }
    if (g_settings_loaded) SaveSettings(); if (log) Log(L"图片组已刷新：" + std::to_wstring(groups.size()) + L" 个组");
}

static HWND g_picker = nullptr;
static HWND g_hover = nullptr;
static POINT g_virtual_origin{};
static bool g_picker_flash = false;
static constexpr UINT_PTR PICKER_FLASH_TIMER = 0xF1D1;
static HWND WindowAtPoint(HWND overlay, POINT pt) {
    ShowWindow(overlay, SW_HIDE);
    HWND h = WindowFromPoint(pt);
    if (h && h != g_hwnd) h = GetAncestor(h, GA_ROOT);
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    return h;
}
static LRESULT CALLBACK PickerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == PICKER_FLASH_TIMER) {
        POINT p{};
        GetCursorPos(&p);
        g_hover = WindowAtPoint(h, p);
        g_picker_flash = !g_picker_flash;
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    if (m == WM_MOUSEMOVE || m == WM_LBUTTONUP) {
        POINT p{}; GetCursorPos(&p); g_hover = WindowAtPoint(h, p); InvalidateRect(h, nullptr, TRUE);
        if (m == WM_LBUTTONUP) { g_target = g_hover; SetWindowTextW(GetDlgItem(g_hwnd, ID_HWND), g_target ? HwndText(g_target).c_str() : L""); SaveSettings(); KillTimer(h, PICKER_FLASH_TIMER); DestroyWindow(h); g_picker = nullptr; SetCursor(LoadCursor(nullptr, IDC_ARROW)); Log(g_target ? L"已选择目标窗口：" + HwndText(g_target) : L"未选择到窗口"); }
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps); RECT client{}; GetClientRect(h, &client); HBRUSH fill = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &client, fill); DeleteObject(fill);
        if (g_hover && IsWindow(g_hover)) {
            RECT r{}; GetWindowRect(g_hover, &r); OffsetRect(&r, -g_virtual_origin.x, -g_virtual_origin.y);
            const COLORREF color = g_picker_flash ? RGB(255, 48, 38) : RGB(255, 215, 48);
            const int width = g_picker_flash ? 4 : 2;
            HPEN pen = CreatePen(g_picker_flash ? PS_SOLID : PS_DOT, width, color);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            if (g_picker_flash) {
                InflateRect(&r, -6, -6);
                Rectangle(dc, r.left, r.top, r.right, r.bottom);
            }
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        }
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { KillTimer(h, PICKER_FLASH_TIMER); DestroyWindow(h); g_picker = nullptr; SetCursor(LoadCursor(nullptr, IDC_ARROW)); Log(L"窗口选择已取消"); return 0; }
    if (m == WM_DESTROY) { KillTimer(h, PICKER_FLASH_TIMER); ReleaseCapture(); g_hover = nullptr; return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void PickWindow() {
    if (g_picker) return;
    static bool registered = false;
    if (!registered) { WNDCLASSW wc{}; wc.hInstance = g_inst; wc.lpfnWndProc = PickerProc; wc.lpszClassName = L"FindImgPicker"; wc.hCursor = LoadCursor(nullptr, IDC_CROSS); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&wc); registered = true; }
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN), w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN); g_virtual_origin = { x, y };
    g_picker = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"FindImgPicker", L"选择窗口", WS_POPUP, x, y, w, h, g_hwnd, nullptr, g_inst, nullptr);
    SetLayeredWindowAttributes(g_picker, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(g_picker, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_picker);
    SetCapture(g_picker);
    POINT p{}; GetCursorPos(&p); g_hover = WindowAtPoint(g_picker, p);
    g_picker_flash = true;
    SetTimer(g_picker, PICKER_FLASH_TIMER, 300, nullptr);
    InvalidateRect(g_picker, nullptr, TRUE);
    SetCursor(LoadCursor(nullptr, IDC_CROSS));
    Log(L"窗口选择已启动：红黄闪烁框会跟随鼠标指向的窗口，单击选择，按 Esc 取消");
}

// JadeView 的“寻找窗口”采用瞄准镜拖拽：按住按钮，拖到目标窗口后松开。
// 高亮框是独立且鼠标穿透的顶层窗口，因此不会遮住 WindowFromPoint 的目标。
static HWND g_drag_highlight = nullptr;
static std::atomic<bool> g_drag_pick_active(false);
static std::thread g_drag_pick_worker;
static std::mutex g_drag_pick_mutex;
static HWND g_drag_pick_hover = nullptr;
static LRESULT CALLBACK DragHighlightProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps); RECT r{}; GetClientRect(h, &r);
        HBRUSH fill = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &r, fill); DeleteObject(fill);
        HPEN pen = CreatePen(PS_SOLID, 5, RGB(255, 48, 48));
        HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, 2, 2, r.right - 2, r.bottom - 2);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static HWND CreateDragHighlight() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{}; wc.hInstance = g_inst; wc.lpfnWndProc = DragHighlightProc;
        wc.lpszClassName = L"FindImgDragHighlight"; RegisterClassW(&wc); registered = true;
    }
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"FindImgDragHighlight", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    if (h) SetLayeredWindowAttributes(h, RGB(0, 0, 0), 0, LWA_COLORKEY);
    return h;
}
static void ShowDragHighlight(HWND h, const RECT& target) {
    if (!h) return;
    constexpr int border = 5;
    SetWindowPos(h, HWND_TOPMOST, target.left - border, target.top - border,
        target.right - target.left + border * 2, target.bottom - target.top + border * 2,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}
static void DestroyDragHighlight() { if (g_drag_highlight) { DestroyWindow(g_drag_highlight); g_drag_highlight = nullptr; } }
static std::wstring PickWindowByDrag() {
    if (g_drag_pick_active.exchange(true)) return L"";
    g_drag_highlight = CreateDragHighlight();
    const DWORD waitStart = GetTickCount();
    while (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        if (GetTickCount() - waitStart > 3000) { DestroyDragHighlight(); g_drag_pick_active = false; return L""; }
        Sleep(10);
    }
    bool flashOn = true; DWORD lastFlash = 0; HWND target = nullptr;
    while (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        POINT p{}; GetCursorPos(&p);
        if (g_drag_highlight) ShowWindow(g_drag_highlight, SW_HIDE);
        target = WindowFromPoint(p);
        if (target && target != g_hwnd) target = GetAncestor(target, GA_ROOT);
        const DWORD now = GetTickCount(); if (now - lastFlash >= 250) { flashOn = !flashOn; lastFlash = now; }
        if (target && flashOn) { RECT r{}; GetWindowRect(target, &r); ShowDragHighlight(g_drag_highlight, r); }
        else if (g_drag_highlight) ShowWindow(g_drag_highlight, SW_HIDE);
        Sleep(20);
    }
    POINT p{}; GetCursorPos(&p);
    if (g_drag_highlight) ShowWindow(g_drag_highlight, SW_HIDE);
    HWND result = WindowFromPoint(p);
    if (result && result != g_hwnd) result = GetAncestor(result, GA_ROOT);
    DestroyDragHighlight(); g_drag_pick_active = false;
    if (result && IsWindow(result)) {
        g_target = result;
        FLASHWINFO fi{ sizeof(fi), result, FLASHW_ALL, 3, 100 }; FlashWindowEx(&fi);
        Log(L"拖拽选择目标窗口：" + HwndText(result));
        return HwndText(result);
    }
    Log(L"拖拽选择窗口已取消：未获取有效目标");
    return L"";
}
static bool BeginWindowDragPick() {
    if (g_drag_pick_active.exchange(true)) return false;
    if (g_drag_pick_worker.joinable()) g_drag_pick_worker.join();
    { std::lock_guard<std::mutex> lock(g_drag_pick_mutex); g_drag_pick_hover = nullptr; }
    Log(L"拖拽瞄准镜已启动：保持鼠标左键拖到目标窗口，松开即可选择");
    g_drag_pick_worker = std::thread([] {
        g_drag_highlight = CreateDragHighlight();
        bool flashOn = true; DWORD lastFlash = 0;
        while (g_drag_pick_active) {
            POINT p{}; GetCursorPos(&p);
            if (g_drag_highlight) ShowWindow(g_drag_highlight, SW_HIDE);
            HWND target = WindowFromPoint(p);
            if (target && target != g_hwnd) target = GetAncestor(target, GA_ROOT);
            { std::lock_guard<std::mutex> lock(g_drag_pick_mutex); g_drag_pick_hover = target; }
            DWORD now = GetTickCount(); if (now - lastFlash >= 250) { flashOn = !flashOn; lastFlash = now; }
            if (target && flashOn) { RECT r{}; GetWindowRect(target, &r); ShowDragHighlight(g_drag_highlight, r); }
            else if (g_drag_highlight) ShowWindow(g_drag_highlight, SW_HIDE);
            Sleep(20);
        }
        DestroyDragHighlight();
    });
    return true;
}
static std::wstring FinishWindowDragPick() {
    if (!g_drag_pick_active.exchange(false)) return L"";
    if (g_drag_pick_worker.joinable()) g_drag_pick_worker.join();
    HWND result = nullptr; { std::lock_guard<std::mutex> lock(g_drag_pick_mutex); result = g_drag_pick_hover; }
    if (result && IsWindow(result)) {
        g_target = result;
        FLASHWINFO fi{ sizeof(fi), result, FLASHW_ALL, 3, 100 }; FlashWindowEx(&fi);
        Log(L"拖拽选择目标窗口：" + HwndText(result));
        return HwndText(result);
    }
    Log(L"拖拽选择窗口已取消：未获取有效目标");
    return L"";
}
static void SelectScheme(int n, bool log = true, bool persist = true) {
    if (log && n == g_scheme) n = 0;
    g_scheme = (std::max)(0, (std::min)(10, n));
    for (auto& [id, h] : g_scheme_controls) {
        LONG style = GetWindowLongW(h, GWL_STYLE);
        SetWindowLongW(h, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_AUTOCHECKBOX);
        SendMessageW(h, BM_SETCHECK, id == g_scheme ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (log) {
        static const wchar_t* names[] = { L"旧版全屏找图", L"PrintWindow 完整渲染", L"PrintWindow 兼容渲染", L"WM_PRINT 控件渲染", L"窗口 DC 位图", L"客户区 DC 位图", L"屏幕窗口区域", L"DWM 合成缩略图", L"DWM 扩展边界截图", L"Windows Graphics Capture", L"UE/游戏专用捕获" };
        if (persist && g_startup_complete) SaveSettings();
        Log(L"已选择截图方案 " + std::to_wstring(g_scheme) + L"：" + names[g_scheme]);
    }
}

static bool LoadTemplate(const fs::path& path, Template& t) {
    Bitmap bmp(path.wstring().c_str(), FALSE); if (bmp.GetLastStatus() != Ok) return false;
    t.w = (int)bmp.GetWidth(); t.h = (int)bmp.GetHeight(); if (!t.w || !t.h) return false;
    Rect r(0, 0, t.w, t.h); BitmapData d{}; if (bmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &d) != Ok) return false;
    std::vector<uint32_t> px((size_t)t.w * t.h); for (int y = 0; y < t.h; ++y) memcpy(px.data() + y * t.w, (BYTE*)d.Scan0 + y * d.Stride, t.w * 4); bmp.UnlockBits(&d);
    uint32_t c0 = px[0] & 0x00ffffff; bool same = true; for (auto p : {px[0], px[t.w - 1], px[(t.h - 1) * t.w], px[t.h * t.w - 1]}) if ((p & 0x00ffffff) != c0) same = false;
    t.gray.resize(px.size()); t.mask.resize(px.size());
    for (size_t i = 0; i < px.size(); ++i) { uint32_t p = px[i]; BYTE b = (BYTE)p, g = (BYTE)(p >> 8), rr = (BYTE)(p >> 16), a = (BYTE)(p >> 24); t.gray[i] = (BYTE)((29 * b + 150 * g + 77 * rr) / 256); t.mask[i] = (a > 0 && (!same || ((p & 0x00ffffff) != c0))) ? 255 : 0; if (t.mask[i]) t.valid.push_back((int)i); }
    if (t.valid.size() < 2) return false; std::sort(t.valid.begin(), t.valid.end());
    for (size_t i = 0; i < t.valid.size() && t.anchors.size() < 6; i += std::max<size_t>(1, t.valid.size() / 6)) t.anchors.push_back(t.valid[i]);
    return true;
}
static Screen Capture() {
    Screen s; s.left = GetSystemMetrics(SM_XVIRTUALSCREEN); s.top = GetSystemMetrics(SM_YVIRTUALSCREEN); s.w = GetSystemMetrics(SM_CXVIRTUALSCREEN); s.h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC dc = GetDC(nullptr), mem = CreateCompatibleDC(dc); BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = s.w; bi.bmiHeader.biHeight = -s.h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB; void* raw = nullptr; HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &raw, nullptr, 0); SelectObject(mem, hb); BitBlt(mem, 0, 0, s.w, s.h, dc, s.left, s.top, SRCCOPY | CAPTUREBLT);
    s.gray.resize((size_t)s.w * s.h); BYTE* p = (BYTE*)raw; for (size_t i = 0; i < s.gray.size(); ++i) s.gray[i] = (BYTE)((29 * p[i * 4] + 150 * p[i * 4 + 1] + 77 * p[i * 4 + 2]) / 256); DeleteObject(hb); DeleteDC(mem); ReleaseDC(nullptr, dc); return s;
}
static Screen CaptureWindow(HWND target) {
    Screen s; RECT r{}; if (!target || !GetWindowRect(target, &r)) return Capture();
    s.left = r.left; s.top = r.top; s.w = r.right - r.left; s.h = r.bottom - r.top; if (s.w <= 0 || s.h <= 0) return Capture();
    int scheme = ActiveScheme();
    if (scheme == 6) {
        Screen full = Capture(); s.gray.resize((size_t)s.w * s.h);
        for (int y = 0; y < s.h; ++y) for (int x = 0; x < s.w; ++x) {
            int sx = s.left - full.left + x, sy = s.top - full.top + y;
            s.gray[(size_t)y * s.w + x] = (sx >= 0 && sy >= 0 && sx < full.w && sy < full.h) ? full.gray[(size_t)sy * full.w + sx] : 0;
        }
        return s;
    }
    HDC dc = GetDC(target), mem = CreateCompatibleDC(dc); BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = s.w; bi.bmiHeader.biHeight = -s.h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB; void* raw = nullptr; HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &raw, nullptr, 0); SelectObject(mem, hb);
    BOOL ok = FALSE; if (scheme <= 3) ok = PrintWindow(target, mem, scheme == 1 ? PW_RENDERFULLCONTENT : 0); if (!ok) { HDC src = scheme == 5 ? GetDC(target) : GetWindowDC(target); BitBlt(mem, 0, 0, s.w, s.h, src, 0, 0, SRCCOPY); ReleaseDC(target, src); }
    s.gray.resize((size_t)s.w * s.h); BYTE* p = (BYTE*)raw; for (size_t i = 0; i < s.gray.size(); ++i) s.gray[i] = (BYTE)((29 * p[i * 4] + 150 * p[i * 4 + 1] + 77 * p[i * 4 + 2]) / 256); DeleteObject(hb); DeleteDC(mem); ReleaseDC(target, dc); return s;
}
static double MatchAt(const Screen& s, const Template& t, int x, int y) {
    double mt = 0, ms = 0; size_t i = 0;
    for (int p : t.valid) { if ((i++ & 1023) == 0 && g_stop) return -1.0; int tx = p % t.w, ty = p / t.w; mt += t.gray[p]; ms += s.gray[(y + ty) * s.w + x + tx]; }
    mt /= t.valid.size(); ms /= t.valid.size(); double num = 0, at = 0, as = 0; i = 0;
    for (int p : t.valid) { if ((i++ & 1023) == 0 && g_stop) return -1.0; int tx = p % t.w, ty = p / t.w; double a = t.gray[p] - mt, b = s.gray[(y + ty) * s.w + x + tx] - ms; num += a * b; at += a * a; as += b * b; }
    return at < 1e-6 || as < 1e-6 ? 0 : num / std::sqrt(at * as);
}
static Match Scan(const std::vector<Template>& ts, double threshold, bool verbose) {
    int scheme = ActiveScheme(); bool useWindow = g_target && IsWindow(g_target) && !IsIconic(g_target) && scheme > 0; Screen s = useWindow ? CaptureWindow(g_target) : Capture(); Match best; if (verbose) Log((useWindow ? L"窗口截图 " + HwndText(g_target) : (g_target ? L"全屏截图（HWND 无效或不可用）" : L"全屏截图")) + L"：" + std::to_wstring(s.w) + L"x" + std::to_wstring(s.h) + L"，方案 " + std::to_wstring(scheme));
    for (const auto& t : ts) { if (t.w > s.w || t.h > s.h) continue; double local = -1; int bx = 0, by = 0;
        for (int y = 0; y <= s.h - t.h && !g_stop; ++y) for (int x = 0; x <= s.w - t.w; ++x) { if ((x & 255) == 0 && g_stop) break; bool pass = true; for (int p : t.anchors) { int tx = p % t.w, ty = p / t.w; if (std::abs((int)s.gray[(y + ty) * s.w + x + tx] - (int)t.gray[p]) > 55) { pass = false; break; } } if (!pass) continue; double v = MatchAt(s, t, x, y); if (v > local) local = v, bx = x, by = y; }
        if (verbose) Log(L"[" + t.group + L"] " + t.name + L" 相似度 " + std::to_wstring(local)); if (local > best.sim) best = { true, local, bx + s.left, by + s.top, &t };
    }
    return best;
}
static void MoveAndClick(int x, int y) { SetCursorPos(x, y); mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); SetCursorPos(0, 0); }
static std::wstring Quote(const std::wstring& s) { std::wstring r = L"\""; for (wchar_t c : s) r += c == L'\"' ? L"\\\"" : std::wstring(1, c); return r + L"\""; }
static void SendMail(const Match& m) {
    // QQ SMTP uses STARTTLS on 587; .NET SmtpClient does not support implicit TLS on 465.
    std::wstring ps = L"try { $m=New-Object Net.Mail.MailMessage; $m.From='" + std::wstring(SMTP_USER) + L"'; $m.To.Add('" + std::wstring(MAIL_TO) + L"'); $m.Subject='找图命中提醒'; $m.Body='命中 " + m.t->group + L" / " + m.t->name + L" 相似度 " + std::to_wstring(m.sim) + L"'; $s=New-Object Net.Mail.SmtpClient('smtp.qq.com',587); $s.EnableSsl=$true; $s.Timeout=20000; $s.Credentials=New-Object Net.NetworkCredential('" + std::wstring(SMTP_USER) + L"','" + std::wstring(SMTP_PASS) + L"'); $s.Send($m); exit 0 } catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }";
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command " + Quote(ps); STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; PROCESS_INFORMATION pi{}; std::vector<wchar_t> b(cmd.begin(), cmd.end()); b.push_back(0); if (CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_base.c_str(), &si, &pi)) { WaitForSingleObject(pi.hProcess, 25000); DWORD code = STILL_ACTIVE; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); if (code == 0) Log(L"邮件提醒已发送"); else Log(L"[错误] 邮件发送失败，请检查 QQ 邮箱授权码和网络"); } else Log(L"[错误] 无法启动邮件发送");
}
static void FlashAlert() {
    HWND h = (g_target && IsWindow(g_target)) ? g_target : g_hwnd;
    if (!h) return;
    FLASHWINFO fi{ sizeof(fi), h, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
    FlashWindowEx(&fi);
    MessageBeep(MB_ICONEXCLAMATION);
    Log(L"已触发窗口闪烁提醒");
}
static void DoAction(const Match& m) { bool click = Checked(ID_CLICK), alert = Checked(ID_FLASH) || Checked(ID_EMAIL); if (!click && !alert) { Log(L"命中，但未勾选任何动作"); return; } if (click) { Log(L"命中 [" + m.t->group + L"] " + m.t->name + L" -> 点击"); MoveAndClick(m.x + m.t->w / 2, m.y + m.t->h / 2); } if (alert) { if (Checked(ID_FLASH)) FlashAlert(); if (Checked(ID_EMAIL)) std::thread(SendMail, m).detach(); Log(L"命中 [" + m.t->group + L"] " + m.t->name + L" -> 提醒"); } }

static bool Away() { static POINT last{}; static auto changed = Clock::now(); static bool init = false; POINT p{}; GetCursorPos(&p); if (!init) last = p, init = true; if (p.x != last.x || p.y != last.y) last = p, changed = Clock::now(); double minutes = Number(ID_AWAY, DEFAULT_AWAY_MINUTES); if (minutes < 1) minutes = 1; return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - changed).count() >= (long long)(minutes * 60); }
static int ClockMinutes(const std::wstring& text) { int h = -1, m = -1; if (swscanf_s(text.c_str(), L"%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1; return h * 60 + m; }
static bool ForcedWindow() { int a = ClockMinutes(ReadCtrl(ID_FORCE_START)), b = ClockMinutes(ReadCtrl(ID_FORCE_END)); if (a < 0 || b < 0 || a == b) return false; SYSTEMTIME st{}; GetLocalTime(&st); int now = st.wHour * 60 + st.wMinute; return a < b ? (now >= a && now < b) : (now >= a || now < b); }
static std::vector<Template> Templates() { std::vector<Template> out; for (auto& n : ImageFiles()) { if (IsSqueezeTemplate(n)) continue; std::wstring g = GroupKey(n); if (!g_groups[g]) continue; Template t; t.group = g; t.name = n; if (LoadTemplate(fs::path(g_base) / L"img" / n, t)) out.push_back(std::move(t)); } return out; }
static void ResetStopState() { g_stop = false; }
static bool ValidTargetWindow() { return g_target && IsWindow(g_target) && !IsIconic(g_target); }
static HWND ResolveDmTarget() {
    if (ValidTargetWindow()) return g_target;
    HWND h = FindWindowW(FINDIMG_TARGET_CLASS, FINDIMG_TARGET_TITLE);
    if (h) { g_target = h; if (g_hwnd) { SetWindowTextW(GetDlgItem(g_hwnd, ID_HWND), HwndText(h).c_str()); SaveSettings(); } Log(L"已自动找到默认目标窗口"); }
    return h;
}
static bool IsSqueezeTemplate(const std::wstring& name) { std::wstring stem = fs::path(name).stem().wstring(); return stem.rfind(L"挤线", 0) == 0; }
static bool SqueezeTemplatesEnabled() { for (auto& n : ImageFiles()) if (IsSqueezeTemplate(n)) return true; return false; }
static bool ValidTargetWindow();
static bool FocusTargetWindow() { HWND foreground = GetForegroundWindow(); DWORD currentThread = GetCurrentThreadId(), targetThread = GetWindowThreadProcessId(g_target, nullptr), foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0; if (foregroundThread && foregroundThread != currentThread) AttachThreadInput(currentThread, foregroundThread, TRUE); if (targetThread && targetThread != currentThread) AttachThreadInput(currentThread, targetThread, TRUE); BringWindowToTop(g_target); BOOL ok = SetForegroundWindow(g_target); if (targetThread && targetThread != currentThread) AttachThreadInput(currentThread, targetThread, FALSE); if (foregroundThread && foregroundThread != currentThread) AttachThreadInput(currentThread, foregroundThread, FALSE); return ok || GetForegroundWindow() == g_target; }
static bool SendAltNumber(int n) { if (!ValidTargetWindow()) return false; INPUT in[4]{}; in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU; in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = (WORD)(L'0' + n); in[2] = in[1]; in[2].ki.dwFlags = KEYEVENTF_KEYUP; in[3] = in[0]; in[3].ki.dwFlags = KEYEVENTF_KEYUP; return SendInput(4, in, sizeof(INPUT)) == 4; }
static bool SqueezeHit(double threshold) { if (!ValidTargetWindow() || !SqueezeTemplatesEnabled()) return false; std::vector<Template> ts; for (auto& n : ImageFiles()) if (IsSqueezeTemplate(n)) { Template t; t.group = L"挤线"; t.name = n; if (LoadTemplate(fs::path(g_base) / L"img" / n, t)) ts.push_back(std::move(t)); } if (ts.empty()) return false; Match m = Scan(ts, threshold, false); if (m.ok && m.sim >= threshold) { Log(L"挤线触发：" + m.t->name + L" 相似度 " + std::to_wstring(m.sim)); return true; } return false; }
static bool RunSqueezeStep(int& nextKey) { if (!ValidTargetWindow()) { Log(L"挤线已停止：目标窗口无效或已最小化"); return false; } if (g_squeeze_triggered) { Log(L"挤线已停止：检测到触发图片"); return false; } int key = nextKey; bool focused = FocusTargetWindow(), sent = SendAltNumber(key); bool ok = focused && sent; Log(L"挤线发送 Alt+" + std::to_wstring(key) + (ok ? L" 成功" : L" 失败：目标窗口无效、未获得前台或输入被系统拒绝")); if (!ok) return false; nextKey = nextKey % 3 + 1; return !g_stop; }
static bool KeySimWait(int ms) { for (int elapsed = 0; elapsed < ms && !g_key_stop; elapsed += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50)); return !g_key_stop; }
class DmSoft {
    IDispatch* disp = nullptr;
    bool invoke(const wchar_t* name, VARIANTARG* args, int n, long* result = nullptr) {
        if (!disp) return false; DISPID id{}; LPOLESTR nm = const_cast<LPOLESTR>(name);
        if (FAILED(disp->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, &id))) return false;
        DISPPARAMS dp{ args, nullptr, (UINT)n, 0 }; VARIANT ret{}; VariantInit(&ret);
        HRESULT hr = disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &ret, nullptr, nullptr);
        bool ok = SUCCEEDED(hr); if (result) *result = ok ? (ret.vt == VT_I4 ? ret.lVal : ret.vt == VT_I2 ? ret.iVal : 0) : -1; VariantClear(&ret); return ok;
    }
public:
    ~DmSoft() { if (disp) disp->Release(); }
    void close() { if (disp) { disp->Release(); disp = nullptr; } }
    bool create() {
        CLSID clsid{};
        HRESULT hr = CLSIDFromProgID(L"dm.dmsoft", &clsid);
        if (FAILED(hr)) {
            AppendDiagnostic(std::wstring(L"DmSoft::create - CLSIDFromProgID 失败，COM 类未注册: 0x") + HwndText((HWND)(UINT_PTR)hr));
            return false;
        }
        AppendDiagnostic(L"DmSoft::create - CLSID 获取成功，尝试创建实例");
        IUnknown* unk = nullptr; 
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)&unk);
        if (FAILED(hr)) {
            AppendDiagnostic(std::wstring(L"DmSoft::create - CoCreateInstance 失败: 0x") + HwndText((HWND)(UINT_PTR)hr));
            return false;
        }
        AppendDiagnostic(L"DmSoft::create - CoCreateInstance 成功，查询 IDispatch 接口");
        hr = unk->QueryInterface(IID_IDispatch, (void**)&disp); 
        unk->Release(); 
        if (FAILED(hr)) {
            AppendDiagnostic(std::wstring(L"DmSoft::create - QueryInterface 失败: 0x") + HwndText((HWND)(UINT_PTR)hr));
        } else {
            AppendDiagnostic(L"DmSoft::create - 创建成功");
        }
        return SUCCEEDED(hr); 
    }
    long reg(const wchar_t* code) { VARIANTARG a[2]{}; a[1].vt = VT_BSTR; a[1].bstrVal = SysAllocString(code); a[0].vt = VT_BSTR; a[0].bstrVal = SysAllocString(L""); long r=-1; invoke(L"Reg", a, 2, &r); VariantClear(&a[0]); VariantClear(&a[1]); return r; }
    long bind(HWND h) { VARIANTARG a[6]{}; a[5].vt=VT_I4; a[5].lVal=(LONG)(INT_PTR)h; a[4].vt=VT_BSTR; a[4].bstrVal=SysAllocString(L"normal"); a[3].vt=VT_BSTR; a[3].bstrVal=SysAllocString(L"normal"); a[2].vt=VT_BSTR; a[2].bstrVal=SysAllocString(L"normal"); a[1].vt=VT_BSTR; a[1].bstrVal=SysAllocString(L""); a[0].vt=VT_I4; a[0].lVal=0; long r=-1; invoke(L"BindWindowEx",a,6,&r); for(auto&v:a)VariantClear(&v); return r; }
    long unbind(){ long r=-1; invoke(L"UnBindWindow",nullptr,0,&r); return r; }
    long lastError(){ long r=-1; invoke(L"GetLastError",nullptr,0,&r); return r; }
    long keyChar(const wchar_t* k){ VARIANTARG a{};a.vt=VT_BSTR;a.bstrVal=SysAllocString(k);long r=-1;invoke(L"KeyPressChar",&a,1,&r);VariantClear(&a);return r; }
    long left(){long r=-1;invoke(L"LeftClick",nullptr,0,&r);return r;} long right(){long r=-1;invoke(L"RightClick",nullptr,0,&r);return r;}
};
static void RequestStopKeySimulation() { g_key_stop = true; }
static void StopKeySimulation(bool wait = true) {
    RequestStopKeySimulation();
    if (wait && g_key_worker.joinable() && std::this_thread::get_id() != g_key_worker.get_id()) g_key_worker.join();
    g_key_running = false;
}
static void StartKeySimulation() {
    if (g_key_running) return;
    if (g_key_worker.joinable()) { if (g_key_running) return; g_key_worker.join(); }
    if (!ResolveDmTarget()) { Log(L"按键模拟启动失败：未找到目标窗口，请在界面上点“拖住寻找”选择目标窗口"); return; }
    g_key_stop = false; g_key_running = true; Log(L"按键模拟已启动（方案H：大漠 DM 插件单线程绑定）：2、左键、右键、F，每步间隔 0.5 秒，整轮等待 5 秒");
     g_key_worker = std::thread([] { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); DmSoft dm; if (!dm.create()) { Log(L"按键模拟失败：无法创建 dm.dmsoft COM 对象（请确认 dm.dll 已注册）"); g_key_running=false; CoUninitialize(); if(g_hwnd)PostMessageW(g_hwnd,WM_KEYSIM_DONE,0,0); return; } long reg = dm.reg(FINDIMG_DM_CODE); Log(L"DM 按键注册码结果：返回码=" + std::to_wstring(reg) + L"，错误码=" + std::to_wstring(dm.lastError()) + (reg == 1 ? L"（注册成功）" : L"（注册失败）")); if (reg != 1) { dm.close(); g_key_running=false; CoUninitialize(); if(g_hwnd)PostMessageW(g_hwnd,WM_KEYSIM_DONE,0,0); return; } wchar_t title[256]{}, cls[256]{}; GetWindowTextW(g_target, title, 256); RealGetWindowClassW(g_target, cls, 256); Log(L"DM 准备绑定：hwnd=" + HwndText(g_target) + L"，title=" + title + L"，class=" + cls + L"，display=normal，mouse=normal，keypad=normal，mode=0"); long bind = dm.bind(g_target); Log(L"DM 绑定结果：返回码=" + std::to_wstring(bind) + L"，错误码=" + std::to_wstring(dm.lastError()) + (bind == 1 ? L"（绑定成功）" : L"（绑定失败）")); if (bind != 1) { dm.close(); g_key_running=false; CoUninitialize(); if(g_hwnd)PostMessageW(g_hwnd,WM_KEYSIM_DONE,0,0); return; } while (!g_key_stop && ValidTargetWindow()) { long r=dm.keyChar(L"2"); if (r != 1) { Log(L"大漠发送 2 失败，返回 " + std::to_wstring(r)); break; } if(!KeySimWait(500))break; r=dm.left(); if (r != 1) { Log(L"大漠发送左键失败，返回 " + std::to_wstring(r)); break; } if(!KeySimWait(500))break; r=dm.right(); if (r != 1) { Log(L"大漠发送右键失败，返回 " + std::to_wstring(r)); break; } if(!KeySimWait(500))break; r=dm.keyChar(L"f"); if (r != 1) { Log(L"大漠发送 F 失败，返回 " + std::to_wstring(r)); break; } if(!KeySimWait(5000))break; } long unbind = dm.unbind(); Log(L"DM 解绑结果：返回码=" + std::to_wstring(unbind) + (unbind == 1 ? L"（解绑成功）" : L"（解绑失败）")); dm.close(); g_key_running=false; CoUninitialize(); if(g_hwnd)PostMessageW(g_hwnd,WM_KEYSIM_DONE,0,0); });
}
static void RunScan(bool test, int scheme_override = -1) {
    if (scheme_override >= 0) g_test_scheme.store(scheme_override);
    try {
    if (test) { ResetStopState(); g_testing = true; } double threshold = std::clamp(Number(ID_THRESHOLD, .95), .01, 1.0); double cooldown = (std::max)((double)MIN_COOLDOWN, Number(ID_COOLDOWN, 15)); static auto lastAction = Clock::now() - std::chrono::seconds(9999);
    auto ts = Templates();
    if (ts.empty()) Log(L"扫描未执行：没有启用且可读取的图片模板");
    else {
        Log(std::wstring(test ? L"测试扫描开始：" : L"循环扫描开始：") + L"模板 " + std::to_wstring(ts.size()) + L" 张，阈值 " + std::to_wstring(threshold) + L"，截图方案 " + std::to_wstring(ActiveScheme()));
        Match m = Scan(ts, threshold, test);
        if (m.ok && m.sim >= threshold) { Log(L"扫描结果：命中 [" + m.t->group + L"] " + m.t->name + L" 相似度 " + std::to_wstring(m.sim)); if (test || std::chrono::duration<double>(Clock::now() - lastAction).count() >= cooldown) DoAction(m), lastAction = Clock::now(); else Log(L"命中但仍在动作冷却中"); }
        else Log(L"扫描结果：未命中");
    }
    if (test) g_testing = false; PostMessageW(g_hwnd, WM_SCAN_DONE, 0, 0);
    } catch (const std::exception& e) { Log(L"扫描异常：" + Utf8ToWide(e.what())); if (test) g_testing = false; }
    catch (...) { Log(L"扫描异常：未知异常"); if (test) g_testing = false; }
    if (scheme_override >= 0) g_test_scheme.store(-1);
}
static void StartLoop() {
    if (g_running) { Log(L"检测循环已经在运行"); return; }
    if (g_worker.joinable()) { Log(L"正在回收上一轮已经结束的检测线程"); g_worker.join(); }
    ResetStopState(); bool squeezeMode = Checked(ID_SQUEEZE);
    if (squeezeMode && (!ValidTargetWindow() || g_scheme <= 0)) { Log(L"挤线启动失败：必须先选择有效窗口并选择窗口截图方案"); return; }
    if (squeezeMode && !SqueezeTemplatesEnabled()) { Log(L"挤线启动失败：请添加 挤线.bmp 模板"); return; }
    g_squeeze_triggered = false; g_running = true;
    Log(squeezeMode ? L"挤线循环已启动：Alt+1/2/3 每秒循环" : L"检测循环已启动：首轮扫描即将开始");
    g_worker = std::thread([squeezeMode] {
        try {
            Log(L"检测线程已进入循环"); std::thread detector;
            if (squeezeMode) detector = std::thread([] { double threshold = std::clamp(Number(ID_THRESHOLD, .95), .01, 1.0); while (!g_stop && !g_squeeze_triggered) if (SqueezeHit(threshold)) { g_squeeze_triggered = true; g_stop = true; break; } });
            bool away = false, forced = false; int squeezeKey = 1; unsigned long long round = 0;
            while (!g_stop) {
                bool nowAway = Away(), nowForced = ForcedWindow();
                if (nowForced != forced) Log(nowForced ? L"进入强制检测时间段，忽略离开状态" : L"离开强制检测时间段");
                if (nowAway && !away && !nowForced) Log(L"鼠标达到离开判定时间，已暂停检测");
                if ((!nowAway || nowForced) && away && !nowForced) Log(L"检测到鼠标移动，已恢复检测");
                away = nowAway; forced = nowForced;
                if (squeezeMode) { if (!RunSqueezeStep(squeezeKey)) { g_stop = true; break; } }
                else if (!nowAway || nowForced) { Log(L"开始第 " + std::to_wstring(++round) + L" 轮检测"); RunScan(false); }
                int waitMs = squeezeMode ? 1000 : (int)(Number(ID_INTERVAL, 1.0) * 1000); if (waitMs < 50) waitMs = 50;
                for (int elapsed = 0; elapsed < waitMs && !g_stop; elapsed += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            g_stop = true; if (detector.joinable()) detector.join();
        } catch (const std::exception& e) { Log(L"检测线程异常：" + Utf8ToWide(e.what())); }
        catch (...) { Log(L"检测线程异常：未知异常"); }
        g_running = false; Log(L"检测循环已结束"); if (g_hwnd) PostMessageW(g_hwnd, WM_WORKER_DONE, 0, 0);
    });
}
static void CALLBACK StopUiTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) { KillTimer(hwnd, id); if (g_hwnd) SetWindowTextW(GetDlgItem(g_hwnd, ID_START), L"启动"); }
static void StopLoop(bool wait = false) { g_stop = true; Log(L"正在停止检测..."); if (!wait && g_hwnd) SetTimer(g_hwnd, 0x534, 1, StopUiTimer); if (wait && g_worker.joinable()) g_worker.join(); if (wait) g_running = false; }
static HWND Add(HWND p, LPCWSTR cls, LPCWSTR text, DWORD style, int id);
static void AddSchemeTestButtons();
static void EnsureForceControls() { if (!GetDlgItem(g_hwnd, ID_FORCE_START)) { Add(g_hwnd, L"STATIC", L"强制检测时间", SS_LEFT, ID_LABEL_FORCE); Add(g_hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_FORCE_START); Add(g_hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_FORCE_END); } if (!GetDlgItem(g_hwnd, ID_SQUEEZE)) Add(g_hwnd, L"BUTTON", L"挤线", BS_AUTOCHECKBOX, ID_SQUEEZE); }

static void Layout(HWND h) {
    EnsureForceControls();
    if (g_scheme_test_controls.empty()) AddSchemeTestButtons();
    RECT r{}; GetClientRect(h, &r); int w = r.right;
    MoveWindow(GetDlgItem(h, ID_LABEL_THRESHOLD), 8, 4, 38, 20, TRUE); MoveWindow(GetDlgItem(h, ID_THRESHOLD), 48, 3, 52, 22, TRUE);
    MoveWindow(GetDlgItem(h, ID_LABEL_INTERVAL), 106, 4, 58, 20, TRUE); MoveWindow(GetDlgItem(h, ID_INTERVAL), 166, 3, 52, 22, TRUE);
    MoveWindow(GetDlgItem(h, ID_LABEL_COOLDOWN), 224, 4, 58, 20, TRUE); MoveWindow(GetDlgItem(h, ID_COOLDOWN), 284, 3, 52, 22, TRUE);
    MoveWindow(GetDlgItem(h, ID_LABEL_AWAY), 342, 4, 68, 20, TRUE); MoveWindow(GetDlgItem(h, ID_AWAY), 412, 3, 52, 22, TRUE);
    MoveWindow(GetDlgItem(h, ID_CLICK), 15, 45, 105, 26, TRUE); MoveWindow(GetDlgItem(h, ID_FLASH), 135, 45, 105, 26, TRUE); MoveWindow(GetDlgItem(h, ID_EMAIL), 255, 45, 105, 26, TRUE); MoveWindow(GetDlgItem(h, ID_SQUEEZE), 375, 45, 70, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_LABEL_HWND), 15, 80, 90, 22, TRUE); MoveWindow(GetDlgItem(h, ID_HWND), 110, 77, w - 205, 25, TRUE); MoveWindow(GetDlgItem(h, ID_PICK), w - 85, 76, 75, 27, TRUE);
    MoveWindow(GetDlgItem(h, ID_LABEL_FORCE), 15, 110, 100, 22, TRUE); MoveWindow(GetDlgItem(h, ID_FORCE_START), 120, 107, 65, 25, TRUE); MoveWindow(GetDlgItem(h, ID_FORCE_END), 205, 107, 65, 25, TRUE);
    int i = 0; for (auto& [g, c] : g_group_controls) { MoveWindow(c, 15 + (i % 2) * (w / 2 - 12), 145 + (i / 2) * 27, w / 2 - 20, 25, TRUE); ++i; } MoveWindow(GetDlgItem(h, ID_REFRESH), w - 75, 142, 65, 27, TRUE);
    MoveWindow(GetDlgItem(h, ID_LABEL_SCHEME), 15, 250, 75, 22, TRUE);
    for (auto& [id, c] : g_scheme_controls) { int x = 95 + (id - 1) * 35; MoveWindow(c, x, 246, 30, 27, TRUE); if (g_scheme_test_controls.count(id)) MoveWindow(g_scheme_test_controls[id], x, 275, 30, 23, TRUE); }
    MoveWindow(GetDlgItem(h, ID_START), 10, 310, 70, 28, TRUE); MoveWindow(GetDlgItem(h, ID_KEYSIM), 85, 310, 85, 28, TRUE); MoveWindow(GetDlgItem(h, ID_TEST), 175, 310, 70, 28, TRUE); MoveWindow(GetDlgItem(h, ID_OPEN), 250, 310, 80, 28, TRUE); MoveWindow(GetDlgItem(h, ID_CLEAR), 335, 310, 70, 28, TRUE); MoveWindow(GetDlgItem(h, 900), 5, 350, w - 10, r.bottom - 355, TRUE);
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLOREDIT || m == WM_CTLCOLORBTN) { HDC dc=(HDC)w; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(225,230,245)); if(m==WM_CTLCOLOREDIT) { SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(20,22,35)); return (LRESULT)g_edit_brush; } return (LRESULT)g_bg_brush; } if (m == WM_ERASEBKGND) return 1; if (m == WM_PAINT) { PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT r{}; GetClientRect(h,&r); FillRect(dc,&r,g_bg_brush); HPEN p=CreatePen(PS_SOLID,1,RGB(40,45,70)); HGDIOBJ old=SelectObject(dc,p); for(int y=36;y<r.bottom;y+=1){} MoveToEx(dc,0,34,nullptr); LineTo(dc,r.right,34); SelectObject(dc,old); DeleteObject(p); EndPaint(h,&ps); return 0; } if (m == WM_LOG) { auto* s = (std::wstring*)l; HWND log = GetDlgItem(h, 900); int n = GetWindowTextLengthW(log); SendMessageW(log, EM_SETSEL, n, n); SendMessageW(log, EM_REPLACESEL, 0, (LPARAM)(s->c_str())); SendMessageW(log, EM_REPLACESEL, 0, (LPARAM)L"\r\n"); delete s; return 0; } if (m == WM_SIZE) { Layout(h); return 0; } if (m == WM_KEYSIM_DONE) { if (g_key_worker.joinable()) g_key_worker.join(); SetWindowTextW(GetDlgItem(h, ID_KEYSIM), L"按键模拟"); Log(L"按键模拟已停止"); return 0; } if (m == WM_WORKER_DONE) { if (g_worker.joinable()) g_worker.join(); g_running = false; SetWindowTextW(GetDlgItem(h, ID_START), L"启动"); Log(L"已停止"); return 0; } if (m == WM_COMMAND) { int id = LOWORD(w); if (id >= 3000 && id < 4000 && HIWORD(w) == BN_CLICKED) { for (auto& [g, c] : g_group_controls) if (c == (HWND)l) { g_groups[g] = SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; break; } SaveSettings(); } else if (id == ID_REFRESH) RefreshGroups(true); else if (id == ID_PICK) PickWindow(); else if (id >= ID_SCHEME_TEST_BASE + 1 && id <= ID_SCHEME_TEST_BASE + 10) { int n = id - ID_SCHEME_TEST_BASE; if (!g_testing) std::thread([n] { RunScan(true, n); }).detach(); } else if (id >= ID_SCHEME_BASE + 1 && id <= ID_SCHEME_BASE + 10) { int n = id - ID_SCHEME_BASE; bool checked = SendMessageW((HWND)l, BM_GETCHECK, 0, 0) == BST_CHECKED; SelectScheme((g_scheme == n && !checked) ? 0 : n); } else if (id == ID_KEYSIM) { if (g_key_running) { RequestStopKeySimulation(); SetWindowTextW(GetDlgItem(h, ID_KEYSIM), L"按键模拟"); Log(L"按键模拟已停止"); } else { StartKeySimulation(); if (g_key_running) SetWindowTextW(GetDlgItem(h, ID_KEYSIM), L"停止"); } } else if (id == ID_START) { if (g_running) { StopLoop(false); SetWindowTextW(GetDlgItem(h, ID_START), L"停止"); } else { StartLoop(); SetWindowTextW(GetDlgItem(h, ID_START), g_running ? L"停止" : L"启动"); } } else if (id == ID_TEST && !g_testing) std::thread([] { RunScan(true); }).detach(); else if (id == ID_OPEN) ShellExecuteW(h, L"open", (fs::path(g_base) / L"img").c_str(), nullptr, nullptr, SW_SHOWNORMAL); else if (id == ID_CLEAR) SetWindowTextW(GetDlgItem(h, 900), L""); else if (id == ID_THRESHOLD || id == ID_INTERVAL || id == ID_COOLDOWN || id == ID_AWAY || id == ID_HWND || id == ID_SQUEEZE || id == ID_CLICK || id == ID_FLASH || id == ID_EMAIL) { if (id == ID_HWND) g_target = ParseHwnd(ReadCtrl(ID_HWND)); SaveSettings(); } return 0; } if (m == WM_CLOSE) { StopKeySimulation(); StopLoop(true); SaveSettings(); DestroyWindow(h); return 0; } if (m == WM_DESTROY) { PostQuitMessage(0); return 0; } return DefWindowProcW(h, m, w, l); }
static LRESULT CALLBACK TransparentStaticProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps); RECT r{}; GetClientRect(h, &r); FillRect(dc, &r, g_bg_brush ? g_bg_brush : GetSysColorBrush(COLOR_WINDOW)); HFONT oldFont = g_ui_font ? (HFONT)SelectObject(dc, g_ui_font) : nullptr; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(225,230,245)); wchar_t text[256]{}; GetWindowTextW(h, text, 256); DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); if (oldFont) SelectObject(dc, oldFont); EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static LRESULT CALLBACK TransparentCheckProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps); RECT r{}; GetClientRect(h, &r); FillRect(dc, &r, g_bg_brush ? g_bg_brush : GetSysColorBrush(COLOR_WINDOW)); HFONT oldFont = g_ui_font ? (HFONT)SelectObject(dc, g_ui_font) : nullptr;
        RECT box{ 2, (r.bottom - 13) / 2, 15, (r.bottom - 13) / 2 + 13 }; UINT state = DFCS_BUTTONCHECK; if (SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED) state |= DFCS_CHECKED; DrawFrameControl(dc, &box, DFC_BUTTON, state);
        wchar_t text[256]{}; GetWindowTextW(h, text, 256); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(225,230,245)); RECT tr{ 19, 0, r.right, r.bottom }; DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); if (oldFont) SelectObject(dc, oldFont); EndPaint(h, &ps); return 0;
    }
    if (m == WM_LBUTTONUP || (m == WM_KEYUP && w == VK_SPACE)) {
        bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(h, BM_SETCHECK, checked ? BST_UNCHECKED : BST_CHECKED, 0);
        InvalidateRect(h, nullptr, TRUE);
        HWND parent = GetParent(h); if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static HWND Add(HWND p, LPCWSTR cls, LPCWSTR text, DWORD style, int id) { if (!g_ui_font) g_ui_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI"); HWND h = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, p, (HMENU)(INT_PTR)id, g_inst, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)g_ui_font, TRUE); if (wcscmp(cls, L"STATIC") == 0) { SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_TRANSPARENT); SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)TransparentStaticProc); } else if (wcscmp(cls, L"EDIT") == 0) { SetWindowTheme(h, L"", L""); } else if (wcscmp(cls, L"BUTTON") == 0) { SetWindowTheme(h, L"", L""); } return h; }
static void AddSchemeTestButtons() { for (int i = 1; i <= 10; ++i) g_scheme_test_controls[i] = Add(g_hwnd, L"BUTTON", L"测", BS_PUSHBUTTON, ID_SCHEME_TEST_BASE + i); }
static void RegisterDmAtStartup() {
    AppendDiagnostic(L"DM COM 启动初始化开始");
    HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); bool coOwned = SUCCEEDED(ci) || ci == S_FALSE;
    if (!coOwned) { Log(L"DM 启动 Reg/Bind 跳过：COM 初始化失败，HRESULT=0x" + HwndText((HWND)(UINT_PTR)ci)); return; }
    DmSoft dm; if (!dm.create()) { Log(L"DM 启动 Reg/Bind 失败：无法创建 dm.dmsoft 对象"); CoUninitialize(); return; }
    long reg = dm.reg(FINDIMG_DM_CODE);
    Log(L"DM 启动 Reg 结果：返回码=" + std::to_wstring(reg) + L"，错误码=" + std::to_wstring(dm.lastError()) + (reg == 1 ? L"（注册成功）" : L"（注册失败）"));
    // Registration is safe at startup; binding is deferred until key simulation.
    // Binding during startup can race the WebView/target window and crash the host.
    if (reg == 1) Log(L"DM 启动初始化完成：COM 与注册码有效，等待按键模拟绑定窗口");
    else if (reg == -2) Log(L"DM 启动初始化失败：Reg 返回 -2，进程权限不足，请以管理员身份运行");
    else Log(L"DM 启动初始化完成，但注册码未通过，Reg 返回码=" + std::to_wstring(reg));
    dm.close();
    CoUninitialize();
}
#ifndef JADE_FRONTEND
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) { g_inst = hi; wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH); g_base = fs::path(buf).parent_path().wstring(); SetProcessDPIAware(); GdiplusStartupInput in; GdiplusStartup(&g_gdiplus, &in, nullptr); WNDCLASSW wc{}; wc.hInstance = hi; wc.lpfnWndProc = WndProc; wc.lpszClassName = L"FindImgCpp"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc); g_hwnd = CreateWindowW(wc.lpszClassName, L"全屏找图工具", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 480, 650, nullptr, nullptr, hi, nullptr); Add(g_hwnd, L"STATIC", L"阈值", SS_LEFT, ID_LABEL_THRESHOLD); Add(g_hwnd, L"STATIC", L"间隔(秒)", SS_LEFT, ID_LABEL_INTERVAL); Add(g_hwnd, L"STATIC", L"冷却(秒)", SS_LEFT, ID_LABEL_COOLDOWN); Add(g_hwnd, L"STATIC", L"离开(分钟)", SS_LEFT, ID_LABEL_AWAY); Add(g_hwnd, L"STATIC", L"目标 HWND", SS_LEFT, ID_LABEL_HWND); Add(g_hwnd, L"EDIT", L"0.95", WS_BORDER | ES_AUTOHSCROLL, ID_THRESHOLD); Add(g_hwnd, L"EDIT", L"1.0", WS_BORDER | ES_AUTOHSCROLL, ID_INTERVAL); Add(g_hwnd, L"EDIT", L"15", WS_BORDER | ES_AUTOHSCROLL, ID_COOLDOWN); Add(g_hwnd, L"EDIT", L"60", WS_BORDER | ES_AUTOHSCROLL, ID_AWAY); Add(g_hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_HWND); Add(g_hwnd, L"BUTTON", L"点击目标", BS_AUTOCHECKBOX, ID_CLICK); Add(g_hwnd, L"BUTTON", L"全屏闪烁", BS_AUTOCHECKBOX, ID_FLASH); Add(g_hwnd, L"BUTTON", L"邮件提醒", BS_AUTOCHECKBOX, ID_EMAIL); Add(g_hwnd, L"BUTTON", L"选择窗口", BS_PUSHBUTTON, ID_PICK); Add(g_hwnd, L"STATIC", L"截图方案", SS_LEFT, ID_LABEL_SCHEME); for (int i = 1; i <= 10; ++i) { HWND b = Add(g_hwnd, L"BUTTON", std::to_wstring(i).c_str(), BS_AUTORADIOBUTTON | (i == 1 ? WS_GROUP : 0), ID_SCHEME_BASE + i); g_scheme_controls[i] = b; } Add(g_hwnd, L"BUTTON", L"刷新", BS_PUSHBUTTON, ID_REFRESH); Add(g_hwnd, L"BUTTON", L"启动", BS_PUSHBUTTON, ID_START); Add(g_hwnd, L"BUTTON", L"按键模拟", BS_PUSHBUTTON, ID_KEYSIM); Add(g_hwnd, L"BUTTON", L"测试", BS_PUSHBUTTON, ID_TEST); Add(g_hwnd, L"BUTTON", L"图片目录", BS_PUSHBUTTON, ID_OPEN); Add(g_hwnd, L"BUTTON", L"清日志", BS_PUSHBUTTON, ID_CLEAR); Add(g_hwnd, L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 900); RefreshGroups(); LoadSettings(); Layout(g_hwnd); RegisterDmAtStartup(); SelectScheme(g_scheme, false, false); g_startup_complete = true; ShowWindow(g_hwnd, show); UpdateWindow(g_hwnd); MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) TranslateMessage(&msg), DispatchMessageW(&msg); GdiplusShutdown(g_gdiplus); return 0; }





#endif // JADE_FRONTEND



