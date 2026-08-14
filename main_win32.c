#ifndef _WIN32
#error "main_win32.c is intended for Windows builds."
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "runtime.h"
#include "net.h"
#include "graphics.c"
#include "net.c"

#define DEFAULT_WIDTH 960
#define DEFAULT_HEIGHT 540

static int g_running = 1, g_script_active = 0, g_fullscreen = 0;
static WINDOWPLACEMENT g_wp_prev = { sizeof(g_wp_prev) };
static uint64_t g_restart_after_ns = 0;
static unsigned int g_restart_failures = 0;
static LARGE_INTEGER g_perf_freq, g_prev_counter;
static HWND g_hwnd = NULL;
static Buffer g_frame = {0};
static int g_client_w = DEFAULT_WIDTH, g_client_h = DEFAULT_HEIGHT;
static int g_key_w = 0, g_key_s = 0, g_key_a = 0, g_key_d = 0, g_key_space = 0, g_mouse_down = 0;

static uint64_t get_time_ns(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000000ULL) / g_perf_freq.QuadPart);
}

static void p_init(void *u) { (void)u; init(NULL); }
static void p_reset(void *u) { (void)u; reset(); }
static void p_update(void *u) { (void)u; update(); }
static void p_draw(void *u) { draw((Buffer *)u); }
typedef struct { float x, y; int action, id; } TouchCall;
static void p_touch(void *u) { TouchCall *c = (TouchCall *)u; touch(c->x, c->y, c->action, c->id); }

static void mark_script_failed(const char *hook) {
    ds_console_log(1, "script error in '%s': %s", hook ? hook : "?", ds_runtime_error_message());
    unsigned int shift = g_restart_failures < 5 ? g_restart_failures : 5;
    g_script_active = 0; ds_request_script_restart();
    g_restart_after_ns = get_time_ns() + (1000000000ULL << shift);
    ++g_restart_failures;
}

static int start_script(int rst) {
    ds_clear_runtime_error(); ds_clear_script_restart(); ds_string_pool_reset();
    if (rst && !ds_call_protected(p_reset, NULL, "reset")) { mark_script_failed("reset"); return 0; }
    if (!ds_call_protected(p_init, NULL, "init")) { mark_script_failed("init"); return 0; }
    g_restart_failures = 0; g_script_active = 1; return 1;
}

static void toggle_fullscreen(HWND hwnd) {
    DWORD dwStyle = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd, &g_wp_prev) && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_wp_prev);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
}

static void update_movement(void) {
    int mx = (g_key_d || (GetAsyncKeyState(VK_RIGHT) & 0x8000)) - (g_key_a || (GetAsyncKeyState(VK_LEFT) & 0x8000));
    int my = (g_key_s || (GetAsyncKeyState(VK_DOWN) & 0x8000)) - (g_key_w || (GetAsyncKeyState(VK_UP) & 0x8000));
    if (mx != 0 || my != 0) {
        float fx = (float)mx, fy = (float)my, len = sqrtf(fx * fx + fy * fy);
        if (len > 0.001f) { fx /= len; fy /= len; }
        joy.dx = fx; joy.dy = fy; joy.ox = fx * joy.r; joy.oy = fy * joy.r;
    } else if (!g_mouse_down) { joy.dx = joy.dy = joy.ox = joy.oy = 0; }
}

static void to_game_coords(int cx, int cy, float *gx, float *gy) {
    *gx = g_client_w > 0 ? (float)cx * (float)screen_w / (float)g_client_w : (float)cx;
    *gy = g_client_h > 0 ? (float)cy * (float)screen_h / (float)g_client_h : (float)cy;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: g_client_w = LOWORD(lParam); g_client_h = HIWORD(lParam); return 0;
        case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP: {
            int act = (msg == WM_LBUTTONDOWN) ? 0 : ((msg == WM_MOUSEMOVE) ? 2 : 1);
            if (msg == WM_LBUTTONDOWN) { g_mouse_down = 1; SetCapture(hwnd); }
            else if (msg == WM_LBUTTONUP) { g_mouse_down = 0; ReleaseCapture(); }
            else if (!g_mouse_down) return 0;
            float gx, gy; to_game_coords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &gx, &gy);
            TouchCall c = { gx, gy, act, 1 };
            if (g_script_active && !ds_call_protected(p_touch, &c, "touch")) mark_script_failed("touch");
            return 0;
        }
        case WM_CHAR: {
            unsigned int ch = (unsigned int)wParam;
            if (ch == VK_BACK) keyboard_backspace();
            else if (ch == VK_RETURN) keyboard_handle_key('\n', 0, 0);
            else if (ch >= 32) {
                char u[5] = {0};
                if (ch < 0x80) { u[0] = (char)ch; }
                else if (ch < 0x800) { u[0] = (char)(0xC0 | (ch >> 6)); u[1] = (char)(0x80 | (ch & 0x3F)); }
                else { u[0] = (char)(0xE0 | (ch >> 12)); u[1] = (char)(0x80 | ((ch >> 6) & 0x3F)); u[2] = (char)(0x80 | (ch & 0x3F)); }
                keyboard_commit_utf8(u);
            }
            return 0;
        }
        case WM_KEYDOWN: case WM_KEYUP: {
            int down = (msg == WM_KEYDOWN);
            switch (wParam) {
                case VK_F11:
                    if (down) toggle_fullscreen(hwnd);
                    return 0;
                case 'W': g_key_w = down; break;
                case 'S': g_key_s = down; break;
                case 'A': g_key_a = down; break;
                case 'D': g_key_d = down; break;
                case VK_SPACE: case 'J': case 'F':
                    if (down && !g_key_space && !keyboard_visible()) {
                        g_key_space = 1; extern double atk_x, atk_y; TouchCall c = { (float)atk_x, (float)atk_y, 0, 99 };
                        if (g_script_active && !ds_call_protected(p_touch, &c, "touch")) mark_script_failed("touch");
                    } else if (!down && g_key_space) {
                        g_key_space = 0; extern double atk_x, atk_y; TouchCall c = { (float)atk_x, (float)atk_y, 1, 99 };
                        if (g_script_active && !ds_call_protected(p_touch, &c, "touch")) mark_script_failed("touch");
                    }
                    break;
                case VK_ESCAPE:
                    if (down) { extern double back_y, btn_w, btn_h; TouchCall c = { (float)((screen_w - btn_w)/2 + btn_w/2), (float)(back_y + btn_h/2), 0, 1 }; if (g_script_active) ds_call_protected(p_touch, &c, "touch"); }
                    break;
            }
            update_movement(); return 0;
        }
        case WM_CLOSE: {
            wchar_t w_msg[128] = {0}, w_title[64] = {0};
            MultiByteToWideChar(CP_UTF8, 0, "Вы точно хотите выйти?", -1, w_msg, 128);
            MultiByteToWideChar(CP_UTF8, 0, "Выход", -1, w_title, 64);
            int res = MessageBoxW(hwnd, w_msg, w_title, MB_YESNO | MB_ICONQUESTION);
            if (res == IDYES) {
                g_running = 0;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY: g_running = 0; PostQuitMessage(0); return 0;
        case WM_ERASEBKGND: return 1;
        default: return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static void render_to_screen(HDC hdc) {
    if (!g_frame.pixels || g_frame.width <= 0) return;
    struct { BITMAPINFOHEADER h; DWORD c[3]; } bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.h.biSize = sizeof(BITMAPINFOHEADER); bmi.h.biWidth = g_frame.width; bmi.h.biHeight = -g_frame.height;
    bmi.h.biPlanes = 1; bmi.h.biBitCount = 32; bmi.h.biCompression = BI_BITFIELDS;
    bmi.c[0] = 0x000000FF; bmi.c[1] = 0x0000FF00; bmi.c[2] = 0x00FF0000;
    StretchDIBits(hdc, 0, 0, g_client_w, g_client_h, 0, 0, g_frame.width, g_frame.height, g_frame.pixels, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
}

int main_loop(HINSTANCE hInst, int cmd) {
    QueryPerformanceFrequency(&g_perf_freq); QueryPerformanceCounter(&g_prev_counter);
    WNDCLASSEXA wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW | CS_OWNDC, WndProc, 0, 0, hInst, NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), NULL, "DimScriptWindowClass", NULL };
    RegisterClassExA(&wc);
    RECT rc = { 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT }; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExA(0, "DimScriptWindowClass", "Cubic Battle (DimScript PC)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left))/2, (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top))/2, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);
    HDC hdc = GetDC(g_hwnd);
    screen_w = DEFAULT_WIDTH; screen_h = DEFAULT_HEIGHT;
    g_frame.width = screen_w; g_frame.height = screen_h; g_frame.stride = screen_w;
    g_frame.pixels = (uint32_t *)calloc((size_t)screen_w * screen_h, sizeof(uint32_t));
    ds_graphics_init(NULL); start_script(0);
    ShowWindow(g_hwnd, cmd); UpdateWindow(g_hwnd);
    MSG msg;
    while (g_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = 0; break; }
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        if (!g_running) break;
        LARGE_INTEGER cur; QueryPerformanceCounter(&cur);
        dt = (double)(cur.QuadPart - g_prev_counter.QuadPart) / (double)g_perf_freq.QuadPart;
        g_prev_counter = cur; if (dt < 0) dt = 0; if (dt > 0.1) dt = 0.1;
        if (!g_script_active && ds_script_restart_requested() && get_time_ns() >= g_restart_after_ns) start_script(1);
        if (g_script_active && !ds_call_protected(p_update, NULL, "update")) mark_script_failed("update");
        if (ds_graphics_begin_frame(&g_frame)) {
            if (g_script_active) {
                if (!ds_call_protected(p_draw, &g_frame, "draw")) mark_script_failed("draw");
                ds_graphics_end_frame();
            } else {
                ds_graphics_error_screen(ds_runtime_error_message());
                ds_graphics_cancel_frame();
            }
        }
        render_to_screen(hdc);
        Sleep(1);
    }
    ds_graphics_shutdown(); free(g_frame.pixels); ReleaseDC(g_hwnd, hdc);
    return 0;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) { (void)p; (void)c; return main_loop(h, s); }
int main(int argc, char **argv) { (void)argc; (void)argv; return main_loop(GetModuleHandle(NULL), SW_SHOWDEFAULT); }
