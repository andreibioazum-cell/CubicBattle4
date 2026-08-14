#ifndef _WIN32
#error "main_win32.c is intended for Windows builds."
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "runtime.h"
#include "net.h"

// Graphics and Net are included in compilation
#include "graphics.c"
#include "net.c"

#define DEFAULT_WIDTH 960
#define DEFAULT_HEIGHT 540
#define WINDOW_TITLE "Cubic Battle (DimScript PC)"

static int g_running = 1;
static int g_script_active = 0;
static uint64_t g_restart_after_ns = 0;
static unsigned int g_restart_failures = 0;
static LARGE_INTEGER g_perf_freq;
static LARGE_INTEGER g_prev_counter;

static HWND g_hwnd = NULL;
static Buffer g_frame = {0};
static int g_client_w = DEFAULT_WIDTH;
static int g_client_h = DEFAULT_HEIGHT;

// Keyboard state tracking
static int g_key_w = 0, g_key_s = 0, g_key_a = 0, g_key_d = 0;
static int g_key_up = 0, g_key_down = 0, g_key_left = 0, g_key_right = 0;
static int g_key_space = 0;
static int g_mouse_down = 0;

static uint64_t get_time_ns(void) {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ULL) / g_perf_freq.QuadPart);
}

static void protected_init(void *userdata) { (void)userdata; init(NULL); }
static void protected_reset(void *userdata) { (void)userdata; reset(); }
static void protected_update(void *userdata) { (void)userdata; update(); }
static void protected_draw(void *userdata) { draw((Buffer *)userdata); }

typedef struct { float x; float y; int action; int id; } TouchCall;
static void protected_touch(void *userdata) {
    TouchCall *call = (TouchCall *)userdata;
    touch(call->x, call->y, call->action, call->id);
}

static void mark_script_failed(const char *hook) {
    const char *message = ds_runtime_error_message();
    ds_console_log(1, "script error: hook '%s' stopped: %s; restarting", hook ? hook : "unknown", message);
    unsigned int shift = g_restart_failures < 5 ? g_restart_failures : 5;
    uint64_t delay = 1000000000ULL << shift;
    g_script_active = 0;
    ds_request_script_restart();
    g_restart_after_ns = get_time_ns() + delay;
    ++g_restart_failures;
}

static int start_script(int reset_state) {
    int ok;
    ds_clear_runtime_error();
    ds_clear_script_restart();
    ds_string_pool_reset();
    if (reset_state) {
        ok = ds_call_protected(protected_reset, NULL, "reset");
        if (!ok) { mark_script_failed("reset"); return 0; }
    }
    ds_clear_runtime_error();
    ok = ds_call_protected(protected_init, NULL, "init");
    if (!ok) { mark_script_failed("init"); return 0; }
    ds_clear_runtime_error();
    g_restart_failures = 0;
    g_script_active = 1;
    return 1;
}

static void restart_script_if_due(void) {
    if (g_script_active || !ds_script_restart_requested()) return;
    uint64_t now = get_time_ns();
    if (now < g_restart_after_ns) return;
    (void)start_script(1);
}

static void update_keyboard_movement(void) {
    int move_x = (g_key_d || g_key_right) - (g_key_a || g_key_left);
    int move_y = (g_key_s || g_key_down) - (g_key_w || g_key_up);

    if (move_x != 0 || move_y != 0) {
        float fx = (float)move_x;
        float fy = (float)move_y;
        float len = sqrtf(fx * fx + fy * fy);
        if (len > 0.001f) {
            fx /= len;
            fy /= len;
        }
        joy.dx = fx;
        joy.dy = fy;
        joy.ox = fx * joy.r;
        joy.oy = fy * joy.r;
    } else if (!g_mouse_down) {
        joy.dx = 0;
        joy.dy = 0;
        joy.ox = 0;
        joy.oy = 0;
    }
}

static void resize_framebuffer(int w, int h) {
    if (w <= 0) w = DEFAULT_WIDTH;
    if (h <= 0) h = DEFAULT_HEIGHT;
    
    screen_w = w;
    screen_h = h;
    
    if (g_frame.pixels) {
        free(g_frame.pixels);
    }
    g_frame.width = w;
    g_frame.height = h;
    g_frame.stride = w;
    g_frame.pixels = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
}

static void screen_to_game_coords(int client_x, int client_y, float *out_x, float *out_y) {
    if (g_client_w > 0 && g_client_h > 0) {
        *out_x = (float)client_x * (float)screen_w / (float)g_client_w;
        *out_y = (float)client_y * (float)screen_h / (float)g_client_h;
    } else {
        *out_x = (float)client_x;
        *out_y = (float)client_y;
    }
}

// Window Procedure
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            g_client_w = LOWORD(lParam);
            g_client_h = HIWORD(lParam);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            g_mouse_down = 1;
            SetCapture(hwnd);
            float gx, gy;
            screen_to_game_coords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &gx, &gy);
            TouchCall call = { gx, gy, 0, 1 };
            if (g_script_active) {
                if (!ds_call_protected(protected_touch, &call, "touch")) mark_script_failed("touch");
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_mouse_down) {
                float gx, gy;
                screen_to_game_coords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &gx, &gy);
                TouchCall call = { gx, gy, 2, 1 };
                if (g_script_active) {
                    if (!ds_call_protected(protected_touch, &call, "touch")) mark_script_failed("touch");
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            g_mouse_down = 0;
            ReleaseCapture();
            float gx, gy;
            screen_to_game_coords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &gx, &gy);
            TouchCall call = { gx, gy, 1, 1 };
            if (g_script_active) {
                if (!ds_call_protected(protected_touch, &call, "touch")) mark_script_failed("touch");
            }
            return 0;
        }
        case WM_CHAR: {
            unsigned int ch = (unsigned int)wParam;
            if (ch == VK_BACK) {
                keyboard_backspace();
            } else if (ch == VK_RETURN) {
                keyboard_handle_key('\n', 0, 0);
            } else if (ch >= 32) {
                char utf8[5] = {0};
                if (ch < 0x80) {
                    utf8[0] = (char)ch;
                } else if (ch < 0x800) {
                    utf8[0] = (char)(0xC0 | (ch >> 6));
                    utf8[1] = (char)(0x80 | (ch & 0x3F));
                } else {
                    utf8[0] = (char)(0xE0 | (ch >> 12));
                    utf8[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    utf8[2] = (char)(0x80 | (ch & 0x3F));
                }
                keyboard_commit_utf8(utf8);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            switch (wParam) {
                case 'W': g_key_w = 1; break;
                case 'S': g_key_s = 1; break;
                case 'A': g_key_a = 1; break;
                case 'D': g_key_d = 1; break;
                case VK_UP: g_key_up = 1; break;
                case VK_DOWN: g_key_down = 1; break;
                case VK_LEFT: g_key_left = 1; break;
                case VK_RIGHT: g_key_right = 1; break;
                case VK_SPACE:
                case 'J':
                case 'F': {
                    if (!g_key_space && !keyboard_visible()) {
                        g_key_space = 1;
                        // Trigger attack button / aiming
                        extern double atk_x, atk_y;
                        TouchCall call = { (float)atk_x, (float)atk_y, 0, 99 };
                        if (g_script_active) {
                            if (!ds_call_protected(protected_touch, &call, "touch")) mark_script_failed("touch");
                        }
                    }
                    break;
                }
                case VK_ESCAPE: {
                    // Back button
                    extern double back_y, btn_w, btn_h;
                    float bx = (float)((screen_w - btn_w) / 2.0 + btn_w / 2.0);
                    float by = (float)(back_y + btn_h / 2.0);
                    TouchCall call = { bx, by, 0, 1 };
                    if (g_script_active) {
                        if (!ds_call_protected(protected_touch, &call, "touch")) mark_script_failed("touch");
                    }
                    break;
                }
            }
            update_keyboard_movement();
            return 0;
        }
        case WM_KEYUP: {
            switch (wParam) {
                case 'W': g_key_w = 0; break;
                case 'S': g_key_s = 0; break;
                case 'A': g_key_a = 0; break;
                case 'D': g_key_d = 0; break;
                case VK_UP: g_key_up = 0; break;
                case VK_DOWN: g_key_down = 0; break;
                case VK_LEFT: g_key_left = 0; break;
                case VK_RIGHT: g_key_right = 0; break;
                case VK_SPACE:
                case 'J':
                case 'F': {
                    if (g_key_space) {
                        g_key_space = 0;
                        extern double atk_x, atk_y;
                        TouchCall call = { (float)atk_x, (float)atk_y, 1, 99 };
                        if (g_script_active) {
                            if (!ds_call_protected(protected_touch, &call, "touch")) mark_script_failed("touch");
                        }
                    }
                    break;
                }
            }
            update_keyboard_movement();
            return 0;
        }
        case WM_CLOSE: {
            g_running = 0;
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: {
            g_running = 0;
            PostQuitMessage(0);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static void render_frame_to_window(HWND hwnd, HDC hdc) {
    if (!g_frame.pixels || g_frame.width <= 0 || g_frame.height <= 0) return;

    // Use BITMAPV5HEADER or BITMAPINFO with BI_BITFIELDS to display RGBA directly
    struct {
        BITMAPINFOHEADER bmiHeader;
        DWORD bmiColors[3];
    } bmi;
    
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_frame.width;
    bmi.bmiHeader.biHeight = -g_frame.height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_BITFIELDS;
    bmi.bmiColors[0] = 0x000000FF; // Red
    bmi.bmiColors[1] = 0x0000FF00; // Green
    bmi.bmiColors[2] = 0x00FF0000; // Blue

    StretchDIBits(
        hdc,
        0, 0, g_client_w, g_client_h,
        0, 0, g_frame.width, g_frame.height,
        g_frame.pixels,
        (BITMAPINFO *)&bmi,
        DIB_RGB_COLORS,
        SRCCOPY
    );
}

int main_loop(HINSTANCE hInstance, int nCmdShow) {
    QueryPerformanceFrequency(&g_perf_freq);
    QueryPerformanceCounter(&g_prev_counter);

    // Register Window Class
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "DimScriptWindowClass";

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class.", "Error", MB_ICONERROR);
        return 1;
    }

    RECT rc = { 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    int screen_cx = GetSystemMetrics(SM_CXSCREEN);
    int screen_cy = GetSystemMetrics(SM_CYSCREEN);
    int win_x = (screen_cx - win_w) / 2;
    int win_y = (screen_cy - win_h) / 2;

    g_hwnd = CreateWindowExA(
        0,
        "DimScriptWindowClass",
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        win_x, win_y, win_w, win_h,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) {
        MessageBoxA(NULL, "Failed to create game window.", "Error", MB_ICONERROR);
        return 1;
    }

    HDC hdc = GetDC(g_hwnd);
    resize_framebuffer(DEFAULT_WIDTH, DEFAULT_HEIGHT);

    if (!ds_graphics_init(NULL)) {
        MessageBoxA(g_hwnd, "Failed to initialize graphics engine.", "Error", MB_ICONERROR);
        return 1;
    }

    ds_log("Cubic Battle PC desktop starting (Windows GDI / Software Renderer)");
    start_script(0);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Main Game Loop
    MSG msg;
    while (g_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_running) break;

        // Calculate dt
        LARGE_INTEGER current_counter;
        QueryPerformanceCounter(&current_counter);
        dt = (double)(current_counter.QuadPart - g_prev_counter.QuadPart) / (double)g_perf_freq.QuadPart;
        g_prev_counter = current_counter;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;

        restart_script_if_due();

        if (g_script_active) {
            if (!ds_call_protected(protected_update, NULL, "update")) {
                mark_script_failed("update");
            } else if (ds_script_restart_requested()) {
                g_script_active = 0;
                g_restart_after_ns = get_time_ns();
            }
        }

        // Draw frame
        if (ds_graphics_begin_frame(&g_frame)) {
            int draw_failed = 0;
            if (g_script_active) {
                if (!ds_call_protected(protected_draw, &g_frame, "draw")) {
                    mark_script_failed("draw");
                    draw_failed = 1;
                } else if (ds_script_restart_requested()) {
                    g_script_active = 0;
                    g_restart_after_ns = get_time_ns();
                }
            }
            if (!g_script_active) {
                if (draw_failed || ds_script_has_error()) {
                    ds_graphics_error_screen(ds_runtime_error_message());
                }
                ds_graphics_cancel_frame();
            } else {
                ds_graphics_end_frame();
            }
        }

        render_frame_to_window(g_hwnd, hdc);

        // Cap frame rate ~60 FPS
        Sleep(1);
    }

    ds_graphics_shutdown();
    if (g_frame.pixels) {
        free(g_frame.pixels);
        g_frame.pixels = NULL;
    }
    ReleaseDC(g_hwnd, hdc);
    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    return main_loop(hInstance, nCmdShow);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return main_loop(GetModuleHandle(NULL), SW_SHOWDEFAULT);
}
