// DiskWidget.cpp — лёгкий виджет дисков для Windows 10/11 (чистый WinAPI + GDI+).
// Возможности: Acrylic/стекло, шкалы (красные > 85%), трей, автообновление 30 с,
// глобальный хоткей F8 (скрыть/показать), автозапуск с Windows, один экземпляр.
//
// Сборка: см. build.bat  (MSVC: cl ... /utf-8   |   MinGW: g++ ...)

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
using std::min;
using std::max;
#include <objidl.h>
#include <gdiplus.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#endif

// ─────────────────────────── Настройки ───────────────────────────
static const int    REFRESH_MS   = 30000;   // автообновление
static const double WARN_PERCENT = 85.0;    // порог красного цвета
static const UINT   HOTKEY_VK    = VK_F8;   // клавиша скрытия/показа (можно поменять)
static const int    BASE_WIDTH   = 340;

// ─────────────────────────── Данные/состояние ───────────────────────────
struct Disk {
    std::wstring root, label;
    ULONGLONG total = 0, freeB = 0;
    double pct = 0;
};

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_DATA = WM_APP + 2;
enum { TIMER_REFRESH = 1, TIMER_DEVICE = 2, HOTKEY_ID = 1 };
enum { CMD_TOGGLE = 1, CMD_REFRESH, CMD_ONTOP, CMD_AUTOSTART, CMD_EXIT };

static HWND g_hwnd = nullptr;
static std::vector<Disk> g_disks;
static std::wstring g_stamp;
static bool g_onTop = false, g_visible = true, g_win11 = false;
static float g_scale = 1.0f;
static std::atomic<bool> g_busy{false};
static NOTIFYICONDATAW g_nid = {};
static UINT g_msgTaskbar = 0;
static HICON g_icon = nullptr;
static ULONG_PTR g_gdipToken = 0;

static const wchar_t* APP_KEY = L"Software\\DiskWidget";
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VAL = L"DiskWidget";

static int S(double v) { return (int)(v * g_scale + 0.5); }

// ─────────────────────────── Реестр: настройки и автозапуск ───────────────────────────
static bool RegGetDword(const wchar_t* name, DWORD& out) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    DWORD sz = sizeof(DWORD), type = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&out, &sz) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(k);
    return ok;
}
static void RegSetDword(const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}
static void SaveSettings() {
    RECT rc;
    if (!g_hwnd || !GetWindowRect(g_hwnd, &rc)) return;
    RegSetDword(L"X", (DWORD)rc.left);
    RegSetDword(L"Y", (DWORD)rc.top);
    RegSetDword(L"OnTop", g_onTop ? 1 : 0);
}
static bool AutostartOn() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, RUN_VAL, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
static void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, RUN_VAL, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VAL);
    }
    RegCloseKey(k);
}

// ─────────────────────────── Сбор информации о дисках ───────────────────────────
static std::vector<Disk> CollectDisks() {
    std::vector<Disk> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;   // только локальные/съёмные
        ULARGE_INTEGER avail, total, freeb;
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &freeb) || total.QuadPart == 0) continue;
        wchar_t label[MAX_PATH + 1] = {};
        GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        Disk d;
        d.root = root;
        d.label = label;
        d.total = total.QuadPart;
        d.freeB = freeb.QuadPart;
        d.pct = (double)(d.total - d.freeB) / (double)d.total * 100.0;
        out.push_back(d);
    }
    return out;
}

static void StartRefresh() {
    if (g_busy.exchange(true)) return;
    HWND h = g_hwnd;
    std::thread([h] {
        auto* v = new std::vector<Disk>(CollectDisks());
        if (!PostMessageW(h, WM_DATA, 0, (LPARAM)v)) delete v;
    }).detach();
}

// ─────────────────────────── Рисование (GDI+ → слоистое окно) ───────────────────────────
static void RoundRectPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    if (r <= 0.5f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void Txt(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& f,
                const Gdiplus::Brush& b, float x, float y, float w, const Gdiplus::StringFormat& sf) {
    g.DrawString(s.c_str(), -1, &f, Gdiplus::RectF(x, y, w, (float)S(20)), &sf, &b);
}

static std::wstring FmtGB(ULONGLONG bytes) {
    wchar_t b[64];
    _snwprintf(b, 63, L"%.1f \u0413\u0411", (double)bytes / 1073741824.0);
    b[63] = 0;
    return b;
}

static void Render() {
    using namespace Gdiplus;
    const int n = (int)g_disks.size();
    const int pad = S(18), headerH = S(46), rowH = S(62);
    const int W = S(BASE_WIDTH);
    const int H = headerH + max(n, 1) * rowH + S(8);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);

    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        // Стеклянная подложка
        GraphicsPath bg;
        RoundRectPath(bg, 0.5f, 0.5f, (float)W - 1, (float)H - 1, g_win11 ? (float)S(8) : 0.f);
        SolidBrush fill(Color(125, 22, 22, 28));
        g.FillPath(&fill, &bg);
        Pen border(Color(50, 255, 255, 255), 1.0f);
        g.DrawPath(&border, &bg);

        FontFamily ff(L"Segoe UI");
        Font fTitle(&ff, (REAL)S(15), FontStyleBold, UnitPixel);
        Font fName(&ff, (REAL)S(13), FontStyleBold, UnitPixel);
        Font fSmall(&ff, (REAL)S(11), FontStyleRegular, UnitPixel);
        Font fTiny(&ff, (REAL)S(10), FontStyleRegular, UnitPixel);

        StringFormat sfL(StringFormat::GenericTypographic());
        sfL.SetFormatFlags(sfL.GetFormatFlags() | StringFormatFlagsNoWrap);
        StringFormat sfR(&sfL);
        sfR.SetAlignment(StringAlignmentFar);

        SolidBrush white(Color(255, 255, 255, 255));
        SolidBrush dim(Color(150, 255, 255, 255));
        SolidBrush red(Color(255, 255, 69, 58));
        SolidBrush blue(Color(255, 76, 194, 255));
        SolidBrush track(Color(34, 255, 255, 255));

        const float cw = (float)(W - 2 * pad);

        // Заголовок
        Txt(g, L"\u0414\u0438\u0441\u043A\u0438", fTitle, white, (float)pad, (float)S(13), cw, sfL);
        Txt(g, g_stamp, fTiny, dim, (float)pad, (float)S(17), cw, sfR);

        if (n == 0) {
            Txt(g, L"\u0417\u0430\u0433\u0440\u0443\u0437\u043A\u0430\u2026", fSmall, dim,
                (float)pad, (float)(headerH + S(4)), cw, sfL);
        }

        for (int i = 0; i < n; i++) {
            const Disk& d = g_disks[i];
            const float y = (float)(headerH + i * rowH);
            const bool warn = d.pct > WARN_PERCENT;

            std::wstring name = d.root.substr(0, 2);
            if (!d.label.empty()) name += L"  " + d.label;
            Txt(g, name, fName, white, (float)pad, y, cw, sfL);

            RectF bounds;
            g.MeasureString(name.c_str(), -1, &fName, PointF(0, 0), &sfL, &bounds);
            Txt(g, L"\u00B7 " + FmtGB(d.total), fSmall, dim,
                (float)pad + bounds.Width + (float)S(8), y + (float)S(2), cw, sfL);

            wchar_t pb[32];
            _snwprintf(pb, 31, L"%.1f%%", d.pct);
            pb[31] = 0;
            Txt(g, pb, fName, warn ? (const Brush&)red : (const Brush&)white, (float)pad, y, cw, sfR);

            // Шкала
            const float bh = (float)S(8), by = y + (float)S(25);
            GraphicsPath tp;
            RoundRectPath(tp, (float)pad, by, cw, bh, bh / 2);
            g.FillPath(&track, &tp);
            float fw = cw * (float)min(100.0, max(0.0, d.pct)) / 100.0f;
            if (fw > 0) {
                fw = max(fw, bh);
                GraphicsPath fp;
                RoundRectPath(fp, (float)pad, by, fw, bh, bh / 2);
                g.FillPath(warn ? (const Brush*)&red : (const Brush*)&blue, &fp);
            }

            // Занято / свободно
            Txt(g, L"\u0417\u0430\u043D\u044F\u0442\u043E " + FmtGB(d.total - d.freeB), fSmall, dim,
                (float)pad, y + (float)S(39), cw, sfL);
            Txt(g, L"\u0421\u0432\u043E\u0431\u043E\u0434\u043D\u043E " + FmtGB(d.freeB), fSmall, dim,
                (float)pad, y + (float)S(39), cw, sfR);
        }
    }

    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    POINT dst = { rc.left, rc.top }, src = { 0, 0 };
    SIZE sz = { W, H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ─────────────────────────── Эффекты Windows (Acrylic, углы) ───────────────────────────
struct ACCENT_POLICY_ { int AccentState; int AccentFlags; unsigned int GradientColor; int AnimationId; };
struct WCA_DATA_ { int Attribute; void* Data; SIZE_T SizeOfData; };
typedef BOOL(WINAPI* SetWCAFn)(HWND, WCA_DATA_*);

static void EnableEffects(HWND h) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto fn = (SetWCAFn)GetProcAddress(u, "SetWindowCompositionAttribute");
    if (fn) {
        ACCENT_POLICY_ a = { 4, 2, 0x40181414u, 0 };   // ACRYLICBLURBEHIND, тон AABBGGRR
        WCA_DATA_ d = { 19, &a, sizeof(a) };           // WCA_ACCENT_POLICY
        fn(h, &d);
    }
    if (g_win11) {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            typedef HRESULT(WINAPI * DwmSetFn)(HWND, DWORD, LPCVOID, DWORD);
            auto f = (DwmSetFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (f) { int pref = 2; f(h, 33, &pref, sizeof(pref)); }   // DWMWCP_ROUND
        }
    }
}

static bool DetectWin11() {
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return fn(&vi) == 0 && vi.dwBuildNumber >= 22000;
}

// ─────────────────────────── Трей, меню, видимость ───────────────────────────
static HICON MakeIcon() {
    using namespace Gdiplus;
    Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    SolidBrush bgb(Color(255, 30, 42, 56));
    GraphicsPath p;
    RoundRectPath(p, 1, 1, 30, 30, 7);
    g.FillPath(&bgb, &p);
    const float widths[3] = { 20, 13, 24 };
    SolidBrush tr(Color(60, 255, 255, 255)), bl(Color(255, 76, 194, 255)), rd(Color(255, 255, 69, 58));
    for (int i = 0; i < 3; i++) {
        float y = 7.f + i * 7.f;
        g.FillRectangle(&tr, 5.f, y, 22.f, 4.f);
        g.FillRectangle(i == 2 ? (Brush*)&rd : (Brush*)&bl, 5.f, y, min(widths[i], 22.f), 4.f);
    }
    HICON h = nullptr;
    bmp.GetHICON(&h);
    return h;
}

static void AddTray() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, L"Disk Widget", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void UpdateTip() {
    std::wstring t = L"Disk Widget";
    for (size_t i = 0; i < g_disks.size(); i++) {
        wchar_t b[48];
        _snwprintf(b, 47, L"%s%.2s %.0f%%", i ? L" \u00B7 " : L"\n", g_disks[i].root.c_str(), g_disks[i].pct);
        b[47] = 0;
        t += b;
    }
    lstrcpynW(g_nid.szTip, t.c_str(), 128);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ApplyZ() {
    SetWindowPos(g_hwnd, g_onTop ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ToggleVisible() {
    g_visible = !g_visible;
    ShowWindow(g_hwnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (g_visible) ApplyZ();
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_TOGGLE,
                g_visible ? L"\u0421\u043A\u0440\u044B\u0442\u044C\tF8" : L"\u041F\u043E\u043A\u0430\u0437\u0430\u0442\u044C\tF8");
    AppendMenuW(m, MF_STRING, CMD_REFRESH, L"\u041E\u0431\u043D\u043E\u0432\u0438\u0442\u044C \u0441\u0435\u0439\u0447\u0430\u0441");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_onTop ? MF_CHECKED : 0), CMD_ONTOP,
                L"\u041F\u043E\u0432\u0435\u0440\u0445 \u0432\u0441\u0435\u0445 \u043E\u043A\u043E\u043D");
    AppendMenuW(m, MF_STRING | (AutostartOn() ? MF_CHECKED : 0), CMD_AUTOSTART,
                L"\u0417\u0430\u043F\u0443\u0441\u043A\u0430\u0442\u044C \u0441 Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"\u0412\u044B\u0445\u043E\u0434");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case CMD_TOGGLE:    ToggleVisible(); break;
    case CMD_REFRESH:   StartRefresh(); break;
    case CMD_ONTOP:     g_onTop = !g_onTop; ApplyZ(); SaveSettings(); break;
    case CMD_AUTOSTART: SetAutostart(!AutostartOn()); break;
    case CMD_EXIT:      DestroyWindow(g_hwnd); break;
    }
}

// ─────────────────────────── Оконная процедура ───────────────────────────
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbar && msg == g_msgTaskbar) { AddTray(); return 0; }   // Explorer перезапущен

    switch (msg) {
    case WM_NCHITTEST:
        return HTCAPTION;                       // перетаскивание за любую область
    case WM_NCLBUTTONDBLCLK:
        return 0;
    case WM_NCRBUTTONUP:
        ShowMenu();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING:
        if (!g_onTop) {                         // держим виджет «на рабочем столе»
            auto* p = (WINDOWPOS*)lp;
            p->hwndInsertAfter = HWND_BOTTOM;
            p->flags &= ~SWP_NOZORDER;
        }
        break;
    case WM_EXITSIZEMOVE:
        SaveSettings();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH) StartRefresh();
        else if (wp == TIMER_DEVICE) { KillTimer(h, TIMER_DEVICE); StartRefresh(); }
        return 0;
    case WM_DEVICECHANGE:                       // флешку вставили/вытащили
        SetTimer(h, TIMER_DEVICE, 1500, nullptr);
        return TRUE;
    case WM_DATA: {
        auto* v = (std::vector<Disk>*)lp;
        g_disks = std::move(*v);
        delete v;
        g_busy = false;
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t b[64];
        _snwprintf(b, 63, L"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u043E %02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        b[63] = 0;
        g_stamp = b;
        Render();
        UpdateTip();
        return 0;
    }
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) ToggleVisible();
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) ToggleVisible();
        else if (lp == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_DESTROY:
        SaveSettings();
        UnregisterHotKey(h, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ─────────────────────────── Точка входа ───────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"DiskWidget_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, dc);
    g_win11 = DetectWin11();

    Gdiplus::GdiplusStartupInput gin;
    Gdiplus::GdiplusStartup(&g_gdipToken, &gin, nullptr);

    // Настройки: позиция, «поверх окон», первый запуск → автозапуск включаем
    DWORD v = 0;
    int x = 0, y = 0;
    bool havePos = false;
    if (RegGetDword(L"X", v)) { x = (int)v; if (RegGetDword(L"Y", v)) { y = (int)v; havePos = true; } }
    if (RegGetDword(L"OnTop", v)) g_onTop = v != 0;
    if (havePos) {
        POINT pt = { x + 10, y + 10 };
        if (!MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) havePos = false;
    }
    if (!havePos) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.right - S(BASE_WIDTH) - S(24);
        y = wa.top + S(24);
    }
    if (!RegGetDword(L"Initialized", v)) {
        SetAutostart(true);
        RegSetDword(L"Initialized", 1);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DiskWidgetWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Disk Widget", WS_POPUP,
                             x, y, S(BASE_WIDTH), S(100), nullptr, nullptr, hInst, nullptr);

    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = MakeIcon();
    AddTray();
    EnableEffects(g_hwnd);

    if (!RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_NOREPEAT, HOTKEY_VK)) {
        MessageBoxW(g_hwnd,
                    L"\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C \u0437\u0430\u043D\u044F\u0442\u044C \u043A\u043B\u0430\u0432\u0438\u0448\u0443 F8 \u2014 \u043E\u043D\u0430 \u043F\u043E\u0434 \u0434\u0440\u0443\u0433\u0438\u043C \u043F\u0440\u0438\u043B\u043E\u0436\u0435\u043D\u0438\u0435\u043C.\n"
                    L"\u0421\u043A\u0440\u044B\u0432\u0430\u0442\u044C \u0432\u0438\u0434\u0436\u0435\u0442 \u043C\u043E\u0436\u043D\u043E \u0447\u0435\u0440\u0435\u0437 \u0442\u0440\u0435\u0439.",
                    L"Disk Widget", MB_OK | MB_ICONINFORMATION);
    }

    SetTimer(g_hwnd, TIMER_REFRESH, REFRESH_MS, nullptr);
    Render();                                   // первый кадр («Загрузка…»)
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    ApplyZ();
    StartRefresh();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_icon) DestroyIcon(g_icon);
    Gdiplus::GdiplusShutdown(g_gdipToken);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
