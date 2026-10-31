/*
 * PT8 - Windows platform layer: window, framebuffer blit, keyboard/mouse,
 * waveOut audio, file dialogs.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#include "app.h"
#include "tracker.h"
#include "platform.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define AUDIO_RATE 48000
#define NUM_BUFS   6

static HWND hwnd;
static CRITICAL_SECTION audioLock;
static HWAVEOUT waveOut;
static WAVEHDR hdrs[NUM_BUFS];
static int16_t *bufs[NUM_BUFS];
static int bufFrames = 512;
static HANDLE audioEvent, audioThread;
static volatile LONG audioRunning;
static bool audioOk, nullAudio;
static volatile LONGLONG nullFrames;
static DWORD lastPos;
static uint64_t posHigh;

static uint32_t fb[SCR_W * SCR_H];
static BITMAPINFO bmi;
static int dstX, dstY, dstW = SCR_W, dstH = SCR_H;
static bool fullscreen;
static WINDOWPLACEMENT prevPlacement = { sizeof(WINDOWPLACEMENT) };
static char cfgPathU8[MAX_PATH * 3];
static wchar_t exeDir[MAX_PATH];
static bool quitting, quitRequested;

/* ------------------------------------------------------------------ */
/* diagnostics: startup log (PT8.log) and crash report (PT8_crash.txt)  */

static FILE *logFile;

static void logf_(const char *fmt, ...)
{
    if (!logFile) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(logFile, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logFile, fmt, ap);
    va_end(ap);
    fputc('\n', logFile);
    fflush(logFile);
}

static FILE *openBesideExe(const wchar_t *name, const wchar_t *mode)
{
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%ls%ls", exeDir, name);
    path[MAX_PATH * 2 - 1] = 0;
    FILE *f = _wfopen(path, mode);
    if (!f) {
        /* folder not writable: use %TEMP% */
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) {
            _snwprintf(path, MAX_PATH * 2, L"%ls%ls", tmp, name);
            path[MAX_PATH * 2 - 1] = 0;
            f = _wfopen(path, mode);
        }
    }
    return f;
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    wchar_t modName[MAX_PATH] = L"?";
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod);
    if (mod) GetModuleFileNameW(mod, modName, MAX_PATH);
    unsigned long long off = mod ? (unsigned long long)((char *)addr - (char *)mod) : 0;
    FILE *f = openBesideExe(L"PT8_crash.txt", L"w");
    if (f) {
        fprintf(f, "PT8 " APP_VERSION " crash\nexception 0x%08lX at %p (%ls + 0x%llX)\n", (unsigned long)code, addr, modName, off);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(f, "access violation %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        fclose(f);
    }
    logf_("CRASH exception 0x%08lX at %p (+0x%llX)", (unsigned long)code, addr, off);
    wchar_t msg[600];
    _snwprintf(msg, 600, L"PT8 si e' chiuso per un errore interno.\n\nEccezione 0x%08lX all'indirizzo +0x%llX\n\n"
                         L"I dettagli sono stati salvati in PT8_crash.txt.", (unsigned long)code, off);
    msg[599] = 0;
    MessageBoxW(NULL, msg, L"PT8 - errore", MB_OK | MB_ICONERROR | MB_TOPMOST);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------------------ */
/* string helpers                                                      */

static void toUtf8(const wchar_t *w, char *out, int size)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL)) out[0] = 0;
}

static void toWide(const char *s, wchar_t *out, int count)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, count)) out[0] = 0;
}

/* ------------------------------------------------------------------ */
/* platform API                                                        */

void plat_lock(void) { EnterCriticalSection(&audioLock); }
void plat_unlock(void) { LeaveCriticalSection(&audioLock); }
bool plat_audio_ok(void) { return audioOk; }
uint32_t plat_ms(void) { return timeGetTime(); }
const char *plat_config_path(void) { return cfgPathU8; }

FILE *plat_fopen(const char *path, const char *mode)
{
    wchar_t wp[MAX_PATH * 2], wm[16];
    toWide(path, wp, MAX_PATH * 2);
    toWide(mode, wm, 16);
    return _wfopen(wp, wm);
}

uint64_t plat_audio_played_frames(void)
{
    if (!audioOk) {
        if (nullAudio) return (uint64_t)nullFrames;
        return replayer_frames_rendered();
    }
    MMTIME t;
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(waveOut, &t, sizeof t) != MMSYSERR_NOERROR) return 0;
    DWORD pos;
    if (t.wType == TIME_SAMPLES) pos = t.u.sample;
    else if (t.wType == TIME_BYTES) pos = t.u.cb / 4;
    else return replayer_frames_rendered();
    if (pos < lastPos && lastPos - pos > 0x80000000u) posHigh += 0x100000000ull;
    lastPos = pos;
    return posHigh + pos;
}

void plat_set_title(const char *title)
{
    wchar_t w[1200];
    toWide(title, w, 1200);
    if (hwnd) SetWindowTextW(hwnd, w);
}

void plat_quit(void)
{
    /* may be called from inside the UI frame: destroy the window from the main loop */
    quitRequested = true;
}

static const wchar_t *dlgFilter(int kind)
{
    switch (kind) {
        case FT_MODULE:
            return L"ProTracker modules (*.mod)\0*.mod;mod.*\0All files (*.*)\0*.*\0";
        case FT_SAMPLE:
            return L"Samples (*.wav;*.iff;*.8svx;*.aif;*.aiff;*.raw;*.smp)\0*.wav;*.iff;*.8svx;*.aif;*.aiff;*.raw;*.smp\0All files (*.*)\0*.*\0";
        default:
            return L"WAV files (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
    }
}

static void releaseMouse(void)
{
    ReleaseCapture();
    app_mouse_button(0, false);
    app_mouse_button(1, false);
}

bool plat_open_dialog(int kind, char *path, int size)
{
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = dlgFilter(kind);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = kind == FT_MODULE ? L"Load module" : L"Load sample";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    releaseMouse();
    BOOL ok = GetOpenFileNameW(&ofn);
    releaseMouse();
    if (!ok) return false;
    toUtf8(file, path, size);
    return path[0] != 0;
}

bool plat_save_dialog(int kind, const char *defName, char *path, int size)
{
    wchar_t file[MAX_PATH * 2];
    toWide(defName ? defName : "", file, MAX_PATH * 2);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = dlgFilter(kind);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrDefExt = kind == FT_MODULE ? L"mod" : L"wav";
    ofn.lpstrTitle = kind == FT_MODULE ? L"Save module" : (kind == FT_WAV_EXPORT ? L"Export song to WAV" : L"Save sample");
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    releaseMouse();
    BOOL ok = GetSaveFileNameW(&ofn);
    releaseMouse();
    if (!ok) return false;
    toUtf8(file, path, size);
    return path[0] != 0;
}

/* ------------------------------------------------------------------ */
/* audio                                                               */

static DWORD WINAPI audioProc(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    int next = 0;
    while (audioRunning) {
        WaitForSingleObject(audioEvent, 50);
        while (audioRunning && (hdrs[next].dwFlags & WHDR_DONE)) {
            EnterCriticalSection(&audioLock);
            mixer_render(bufs[next], bufFrames);
            LeaveCriticalSection(&audioLock);
            waveOutWrite(waveOut, &hdrs[next], sizeof(WAVEHDR));
            next = (next + 1) % NUM_BUFS;
        }
    }
    return 0;
}

static DWORD WINAPI nullAudioProc(LPVOID arg)
{
    (void)arg;
    static int16_t buf[512 * 2];
    DWORD start = timeGetTime();
    LONGLONG done = 0;
    while (audioRunning) {
        Sleep(5);
        LONGLONG target = (LONGLONG)(timeGetTime() - start) * (AUDIO_RATE / 1000);
        while (done + 512 <= target) {
            EnterCriticalSection(&audioLock);
            mixer_render(buf, 512);
            LeaveCriticalSection(&audioLock);
            done += 512;
            nullFrames = done;
        }
    }
    return 0;
}

static void audioStart(void)
{
    bufFrames = cfg.audioBuffer;
    if (bufFrames < 128) bufFrames = 128;
    if (bufFrames > 8192) bufFrames = 8192;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = AUDIO_RATE * 4;
    audioEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    audioRunning = 1;
    if (waveOutOpen(&waveOut, WAVE_MAPPER, &wf, (DWORD_PTR)audioEvent, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
        audioOk = true;
        for (int i = 0; i < NUM_BUFS; i++) {
            bufs[i] = (int16_t *)calloc((size_t)bufFrames * 2, sizeof(int16_t));
            memset(&hdrs[i], 0, sizeof(WAVEHDR));
            hdrs[i].lpData = (LPSTR)bufs[i];
            hdrs[i].dwBufferLength = (DWORD)bufFrames * 4;
            waveOutPrepareHeader(waveOut, &hdrs[i], sizeof(WAVEHDR));
        }
        for (int i = 0; i < NUM_BUFS; i++) {
            EnterCriticalSection(&audioLock);
            mixer_render(bufs[i], bufFrames);
            LeaveCriticalSection(&audioLock);
            waveOutWrite(waveOut, &hdrs[i], sizeof(WAVEHDR));
        }
        audioThread = CreateThread(NULL, 0, audioProc, NULL, 0, NULL);
    } else {
        nullAudio = true;
        audioThread = CreateThread(NULL, 0, nullAudioProc, NULL, 0, NULL);
    }
}

static void audioStop(void)
{
    audioRunning = 0;
    if (audioEvent) SetEvent(audioEvent);
    if (audioThread) { WaitForSingleObject(audioThread, 2000); CloseHandle(audioThread); audioThread = NULL; }
    if (audioOk) {
        waveOutReset(waveOut);
        for (int i = 0; i < NUM_BUFS; i++) waveOutUnprepareHeader(waveOut, &hdrs[i], sizeof(WAVEHDR));
        waveOutClose(waveOut);
        for (int i = 0; i < NUM_BUFS; i++) free(bufs[i]);
        audioOk = false;
    }
    if (audioEvent) { CloseHandle(audioEvent); audioEvent = NULL; }
}

/* ------------------------------------------------------------------ */
/* window                                                              */

static void computeDest(int cw, int ch)
{
    int s = cw / SCR_W < ch / SCR_H ? cw / SCR_W : ch / SCR_H;
    if (s >= 1) {
        dstW = SCR_W * s;
        dstH = SCR_H * s;
    } else {
        double f = (double)cw / SCR_W < (double)ch / SCR_H ? (double)cw / SCR_W : (double)ch / SCR_H;
        dstW = (int)(SCR_W * f);
        dstH = (int)(SCR_H * f);
        if (dstW < 1) dstW = 1;
        if (dstH < 1) dstH = 1;
    }
    dstX = (cw - dstW) / 2;
    dstY = (ch - dstH) / 2;
}

/* blit the last rendered frame (safe to call from WM_PAINT) */
static void blit(HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    computeDest(rc.right, rc.bottom);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r;
    if (dstY > 0) { SetRect(&r, 0, 0, rc.right, dstY); FillRect(dc, &r, black); SetRect(&r, 0, dstY + dstH, rc.right, rc.bottom); FillRect(dc, &r, black); }
    if (dstX > 0) { SetRect(&r, 0, 0, dstX, rc.bottom); FillRect(dc, &r, black); SetRect(&r, dstX + dstW, 0, rc.right, rc.bottom); FillRect(dc, &r, black); }
    SetStretchBltMode(dc, (dstW < SCR_W) ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, dstX, dstY, dstW, dstH, 0, 0, SCR_W, SCR_H, fb, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* run one UI frame and show it (main loop only: the UI may open modal dialogs) */
static void render(void)
{
    app_frame(fb);
    if (quitRequested) return;
    HDC dc = GetDC(hwnd);
    blit(dc);
    ReleaseDC(hwnd, dc);
}

static void mapMouse(LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    int fx = dstW > 0 ? (x - dstX) * SCR_W / dstW : 0;
    int fy = dstH > 0 ? (y - dstY) * SCR_H / dstH : 0;
    app_mouse_move(fx, fy);
}

static void toggleFullscreen(void)
{
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetWindowPlacement(hwnd, &prevPlacement) && GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen = true;
        }
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(hwnd, &prevPlacement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = false;
    }
}

static int keyMods(void)
{
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= KMOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= KMOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= KMOD_ALT;
    if (GetKeyState(VK_RMENU) & 0x8000) m &= ~(KMOD_CTRL | KMOD_ALT);   /* AltGr / right Alt */
    return m;
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CLOSE:
            if (app_request_quit()) DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            blit(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = SCR_W / 2;
            mm->ptMinTrackSize.y = SCR_H / 2;
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)wp;
            int sc = (int)((lp >> 16) & 0xFF);
            bool ext = (lp >> 24) & 1;
            bool rep = (lp >> 30) & 1;
            if (msg == WM_SYSKEYDOWN && vk == VK_RETURN) { if (!rep) toggleFullscreen(); return 0; }
            if (vk == VK_RETURN && fullscreen && (GetKeyState(VK_MENU) & 0x8000)) return 0;
            app_key_down(vk, sc, ext, rep, keyMods());
            return 0;
        }
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            return 0;
        case WM_CHAR:
            if (wp >= 32 && (wp < 0xD800 || wp > 0xDFFF)) app_char((unsigned)wp);
            return 0;
        case WM_MOUSEMOVE:
            mapMouse(lp);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(h); mapMouse(lp); app_mouse_button(0, true);
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture(); mapMouse(lp); app_mouse_button(0, false);
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            SetCapture(h); mapMouse(lp); app_mouse_button(1, true);
            return 0;
        case WM_RBUTTONUP:
            ReleaseCapture(); mapMouse(lp); app_mouse_button(1, false);
            return 0;
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            app_mouse_wheel(d > 0 ? 1 : -1);
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT *nr = (const RECT *)lp;
            SetWindowPos(h, NULL, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_KILLFOCUS:
            app_mouse_button(0, false);
            app_mouse_button(1, false);
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            wchar_t wpath[MAX_PATH * 2];
            if (DragQueryFileW(drop, 0, wpath, MAX_PATH * 2)) {
                char path[MAX_PATH * 6];
                toUtf8(wpath, path, sizeof path);
                SetForegroundWindow(h);
                app_drop_file(path);
            }
            DragFinish(drop);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;

    SetUnhandledExceptionFilter(crashHandler);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    /* folder of the executable (config, log) */
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    wchar_t *slash = wcsrchr(exeDir, L'\\');
    if (slash) slash[1] = 0; else exeDir[0] = 0;
    logFile = openBesideExe(L"PT8.log", L"w");
    {
        OSVERSIONINFOW ov;
        memset(&ov, 0, sizeof ov);
        ov.dwOSVersionInfoSize = sizeof ov;
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        RtlGetVersionFn rgv = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
        if (rgv) rgv(&ov);
        logf_("PT8 " APP_VERSION " starting - Windows %lu.%lu build %lu", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    }

    /* crisp pixels on high DPI displays */
    HMODULE user = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *SetDpiCtx)(HANDLE);
    SetDpiCtx setCtx = user ? (SetDpiCtx)(void *)GetProcAddress(user, "SetProcessDpiAwarenessContext") : NULL;
    if (!setCtx || !setCtx((HANDLE)-4)) SetProcessDPIAware();

    logf_("dpi awareness set");
    timeBeginPeriod(1);
    InitializeCriticalSection(&audioLock);

    /* config file next to the executable */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *dot = wcsrchr(exe, L'.');
    if (dot) wcscpy(dot, L".ini"); else wcscat(exe, L".ini");
    toUtf8(exe, cfgPathU8, sizeof cfgPathU8);

    app_init();
    logf_("app initialised");

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SCR_W;
    bmi.bmiHeader.biHeight = -SCR_H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"PT8Tracker";
    RegisterClassExW(&wc);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ww = work.right - work.left, wh = work.bottom - work.top;
    int scale = 1;
    for (int s = 4; s >= 1; s--) {
        RECT r = { 0, 0, SCR_W * s, SCR_H * s };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        if (r.right - r.left <= ww && r.bottom - r.top <= wh) { scale = s; break; }
    }
    RECT r = { 0, 0, SCR_W * scale, SCR_H * scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int winW = r.right - r.left, winH = r.bottom - r.top;
    hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"PT8 - 8 channel ProTracker", WS_OVERLAPPEDWINDOW,
                           work.left + (ww - winW) / 2, work.top + (wh - winH) / 2, winW, winH,
                           NULL, NULL, inst, NULL);
    if (!hwnd) {
        logf_("CreateWindowEx failed, error %lu", GetLastError());
        MessageBoxW(NULL, L"Impossibile creare la finestra di PT8.", L"PT8 - errore", MB_OK | MB_ICONERROR);
        return 1;
    }
    logf_("window created %dx%d (scale %d)", winW, winH, scale);
    DragAcceptFiles(hwnd, TRUE);

    /* show the window first, so that it appears even if the audio device is slow to open */
    ShowWindow(hwnd, show);
    app_frame(fb);
    UpdateWindow(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
    logf_("window shown (show=%d)", show);

    audioStart();
    logf_(audioOk ? "audio started (waveOut, %d frames per buffer)" : "no audio device - running silent (%d)", bufFrames);

    /* file given on the command line (or via "open with") */
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        char path[MAX_PATH * 6];
        toUtf8(argv[1], path, sizeof path);
        app_open_file(path);
    }
    if (argv) LocalFree(argv);

    LARGE_INTEGER freq, last, nowc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    const double frameTime = 1.0 / 60.0;
    int framesShown = 0;
    while (!quitting) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quitting = true; break; }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (quitting) break;
        QueryPerformanceCounter(&nowc);
        double el = (double)(nowc.QuadPart - last.QuadPart) / freq.QuadPart;
        if (el >= frameTime) {
            last = nowc;
            if (!IsIconic(hwnd)) render();
            if (++framesShown == 60) logf_("running (60 frames drawn)");
            if (quitRequested) { DestroyWindow(hwnd); quitRequested = false; }
        } else {
            DWORD waitMs = (DWORD)((frameTime - el) * 1000.0);
            MsgWaitForMultipleObjects(0, NULL, FALSE, waitMs ? waitMs : 1, QS_ALLINPUT);
        }
    }

    logf_("quitting");
    audioStop();
    app_shutdown();
    logf_("bye");
    if (logFile) fclose(logFile);
    DeleteCriticalSection(&audioLock);
    timeEndPeriod(1);
    return 0;
}
