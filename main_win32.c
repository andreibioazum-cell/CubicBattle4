#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#define KB_JOY_ID 60
#define KB_BACK_ID 61
#define MOUSE_ID 80
static HWND g_hwnd = NULL;
static uint32_t *g_pixels = NULL;
static int g_bw = 0, g_bh = 0;
static int g_running = 1;
static int g_keys[256];
static int g_kb_joy = 0;
static int g_mouse_down = 0;
static int g_fullscreen = 0;
static RECT g_win_rect;
static LARGE_INTEGER g_freq, g_prev;
static int script_active = 0;
static uint64_t restart_after_ms = 0;
static unsigned restart_failures = 0;
static uint64_t now_ms64(void) { return (uint64_t)GetTickCount64(); }
static void protected_init(void *u) { init((AAssetManager *)u); }
static void protected_reset(void *u) { (void)u; reset(); }
static void protected_update(void *u) { (void)u; update(); }
static void protected_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void protected_touch(void *u) {
    TouchCall *c = (TouchCall *)u;
    touch(c->x, c->y, c->action, c->id);
}
static void mark_failed(const char *hook) {
    ds_console_log(1, "script error: hook '%s' stopped: %s; restarting", hook, ds_runtime_error_message());
    unsigned shift = restart_failures < 5 ? restart_failures : 5;
    script_active = 0;
    ds_request_script_restart();
    restart_after_ms = now_ms64() + (1000ull << shift);
    restart_failures++;
}
static int start_script(int reset_state) {
    int ok;
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (reset_state) {
        ok = ds_call_protected(protected_reset, NULL, "reset");
        if (!ok) { mark_failed("reset"); return 0; }
    }
    ds_clear_runtime_error();
    ok = ds_call_protected(protected_init, NULL, "init");
    if (!ok) { mark_failed("init"); return 0; }
    ds_clear_runtime_error();
    restart_failures = 0;
    script_active = 1;
    return 1;
}
static void restart_if_due(void) {
    if (script_active || !ds_script_restart_requested()) return;
    if (now_ms64() < restart_after_ms) return;
    start_script(1);
}
static void ensure_buffer(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    if (cw == g_bw && ch == g_bh && g_pixels) return;
    uint32_t *np = (uint32_t *)realloc(g_pixels, (size_t)cw * (size_t)ch * 4u);
    if (!np) return;
    g_pixels = np;
    g_bw = cw;
    g_bh = ch;
    /* realloc не обнуляет память: без этого после изменения размера окна в
     * кадр попадает мусор (фиолетовые/чёрные пятна и полосы). */
    memset(g_pixels, 0, (size_t)cw * (size_t)ch * 4u);
    screen_w = cw;
    screen_h = ch;
}
static void atk_touch(int action) {
    touch((float)screen_w - 140.0f, (float)screen_h - 150.0f, action, KB_JOY_ID);
}
static void back_touch(void) {
    touch((float)(screen_w - 280) * 0.5f, 64.0f, 0, KB_BACK_ID);
}
static int cp_to_utf8(unsigned int cp, char out[8]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
static unsigned int g_pending_high = 0;
static void handle_char(unsigned int cp) {
    if (!keyboard_visible()) return;
    if (cp == '\r' || cp == '\n') { keyboard_commit_utf8("\n"); return; }
    if (cp == 8) { keyboard_backspace(); return; }
    if (cp >= 0xD800 && cp <= 0xDBFF) { g_pending_high = cp; return; }
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        if (g_pending_high) {
            cp = 0x10000 + ((g_pending_high - 0xD800) << 10) + (cp - 0xDC00);
        } else return;
    }
    g_pending_high = 0;
    if (cp < 0x20 || cp == 0x7F) return;
    char utf8[8];
    int n = cp_to_utf8(cp, utf8);
    if (n > 0) keyboard_commit_utf8(utf8);
}
static void toggle_fullscreen(void) {
    if (!g_fullscreen) {
        GetWindowRect(g_hwnd, &g_win_rect);
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(mon, &mi);
        SetWindowLongPtr(g_hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 1;
    } else {
        SetWindowLongPtr(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, g_win_rect.left, g_win_rect.top,
                     g_win_rect.right - g_win_rect.left,
                     g_win_rect.bottom - g_win_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 0;
    }
}
static void confirm_exit(void) {
    wchar_t w_msg[128];
    MultiByteToWideChar(CP_UTF8, 0, "Вы точно хотите выйти?", -1, w_msg, 128);
    int r = MessageBoxW(g_hwnd, w_msg, L"ZeroHabit", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (r == IDYES) {
        g_running = 0;
        DestroyWindow(g_hwnd);
    }
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        ensure_buffer((int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN: {
        UINT vk = (UINT)wParam;
        int repeat = (lParam & 0x40000000) != 0;
        if (vk == VK_ESCAPE) {
            if (!repeat) back_touch();
            return 0;
        }
        if (vk == VK_F11) {
            if (!repeat) toggle_fullscreen();
            return 0;
        }
        if (vk == VK_SPACE || vk == 'J' || vk == 'F') {
            if (!repeat) atk_touch(0);
            return 0;
        }
        g_keys[vk & 0xFF] = 1;
        return 0;
    }
    case WM_KEYUP: {
        UINT vk = (UINT)wParam;
        g_keys[vk & 0xFF] = 0;
        if (vk == VK_SPACE || vk == 'J' || vk == 'F') atk_touch(1);
        return 0;
    }
    case WM_CHAR:
        handle_char((unsigned int)wParam);
        return 0;
    case WM_LBUTTONDOWN:
        g_mouse_down = 1;
        SetCapture(hwnd);
        touch((float)(short)LOWORD(lParam), (float)(short)HIWORD(lParam), 0, MOUSE_ID);
        return 0;
    case WM_LBUTTONUP:
        g_mouse_down = 0;
        ReleaseCapture();
        touch((float)(short)LOWORD(lParam), (float)(short)HIWORD(lParam), 1, MOUSE_ID);
        return 0;
    case WM_MOUSEMOVE:
        if (g_mouse_down)
            touch((float)(short)LOWORD(lParam), (float)(short)HIWORD(lParam), 2, MOUSE_ID);
        return 0;
    case WM_CLOSE:
        confirm_exit();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
static void render_frame(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double d = (double)(now.QuadPart - g_prev.QuadPart) / (double)g_freq.QuadPart;
    g_prev = now;
    dt = d;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    int left = g_keys['A'] || g_keys[VK_LEFT], right = g_keys['D'] || g_keys[VK_RIGHT];
    int up = g_keys['W'] || g_keys[VK_UP], down = g_keys['S'] || g_keys[VK_DOWN];
    float dx = (float)(right - left), dy = (float)(down - up);
    if (dx != 0.0f || dy != 0.0f) {
        float m = sqrtf(dx * dx + dy * dy);
        if (m > 1.0f) { dx /= m; dy /= m; }
        joy.dx = dx;
        joy.dy = dy;
        joy.ox = dx * joy.r;
        joy.oy = dy * joy.r;
        g_kb_joy = 1;
    } else if (g_kb_joy) {
        g_kb_joy = 0;
        joy.dx = 0; joy.dy = 0; joy.ox = 0; joy.oy = 0;
    }
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0 || !g_pixels) return;
    if (cw != g_bw || ch != g_bh) ensure_buffer(cw, ch);
    screen_w = cw;
    screen_h = ch;
    restart_if_due();
    if (script_active) {
        if (!ds_call_protected(protected_update, NULL, "update")) mark_failed("update");
        else if (ds_script_restart_requested()) { script_active = 0; restart_after_ms = now_ms64(); }
    }
    Buffer frame = { g_pixels, g_bw, g_bh, g_bw };
    if (ds_graphics_begin_frame(&frame)) {
        int draw_failed = 0;
        if (script_active) {
            if (!ds_call_protected(protected_draw, &frame, "draw")) { mark_failed("draw"); draw_failed = 1; }
            else if (ds_script_restart_requested()) { script_active = 0; restart_after_ms = now_ms64(); }
        }
        if (!script_active) {
            if (draw_failed || ds_script_has_error()) ds_graphics_error_screen(ds_runtime_error_message());
            ds_graphics_cancel_frame();
        } else {
            ds_graphics_end_frame();
        }
        /* Игровой буфер хранит пиксели в порядке R,G,B,A (как на Android).
         * GDI-окну нужен 32-битный DIB в порядке B,G,R,X. Меняем местами
         * красный и синий байты и глушим альфу. Раньше здесь были маски
         * BI_BITFIELDS — часть видеодрайверов читает их в порядке B,G,R,
         * из-за чего каналы R/B менялись местами и «цвета ломались»
         * (фиолетовые/чёрные тона). BI_RGB без масок работает везде. */
        if (g_pixels) {
            uint32_t *pp = g_pixels;
            size_t pn = (size_t)g_bw * (size_t)g_bh;
            for (size_t i = 0; i < pn; i++) {
                uint32_t v = pp[i];
                pp[i] = ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu) | 0xFF000000u;
            }
        }
    }
    HDC dc = GetDC(g_hwnd);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_bw;
    bi.bmiHeader.biHeight = -g_bh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    /* BI_RGB 32bpp: байты B,G,R,X (см. swizzle выше). Без BI_BITFIELDS-масок,
     * которые разные драйверы толкуют по-разному. */
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, 0, 0, cw, ch, 0, 0, g_bw, g_bh,
                  g_pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    (void)hPrev;
    (void)cmdLine;
    SetProcessDPIAware();
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    /* Иконка из ресурса с id 1 — её кладёт CMake, когда в корне есть
     * icon.ico. Нет ресурса — берём стандартную иконку Windows. */
    wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "ZeroHabitWindow";
    if (!RegisterClassExA(&wc)) return 1;
    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("ZeroHabitWindow", "ZeroHabit: Clean Slate",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_prev);
    ensure_buffer(1280, 720);
    ds_graphics_init(NULL);
    start_script(0);
    while (g_running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        render_frame();
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) / (double)g_freq.QuadPart * 1000.0;
        if (ms < 15.0) Sleep((DWORD)(15.0 - ms));
    }
    ds_graphics_shutdown();
    free(g_pixels);
    return 0;
}
#else
#include <stdio.h>
int main(void) {
    fprintf(stderr, "This build requires Windows. Use the MSVC/CMake build on Windows.\n");
    return 1;
}
#endif
#include "graphics.c"
#include "net.c"
