/*
 * Windows-side test driver for PT8 (runs under Wine or Windows).
 *   wdriver find                 -> prints window handle, title, rect, visibility
 *   wdriver key <vk> [scancode]  -> posts WM_KEYDOWN/UP to the PT8 window
 *   wdriver click <x> <y>        -> left click at framebuffer coords (720x480 space)
 *   wdriver close                -> posts WM_CLOSE
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HWND findPT8(void) { return FindWindowW(L"PT8Tracker", NULL); }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    HWND h = NULL;
    for (int i = 0; i < 50 && !h; i++) { h = findPT8(); if (!h) Sleep(100); }
    if (!h) { printf("PT8 window NOT FOUND\n"); return 1; }
    if (!strcmp(argv[1], "find")) {
        wchar_t t[256];
        GetWindowTextW(h, t, 256);
        RECT r, c;
        GetWindowRect(h, &r);
        GetClientRect(h, &c);
        printf("hwnd=%p visible=%d iconic=%d title='%ls' rect=%ld,%ld-%ld,%ld client=%ldx%ld\n", (void *)h,
               IsWindowVisible(h), IsIconic(h), t, r.left, r.top, r.right, r.bottom, c.right, c.bottom);
        DWORD_PTR res = 0;
        LRESULT ok = SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &res);
        printf("responsive=%d\n", ok != 0);
        return 0;
    }
    if (!strcmp(argv[1], "key") && argc >= 3) {
        int vk = (int)strtol(argv[2], NULL, 0);
        int sc = argc >= 4 ? (int)strtol(argv[3], NULL, 0) : (int)MapVirtualKeyW(vk, 0);
        PostMessageW(h, WM_KEYDOWN, vk, 1 | (sc << 16));
        Sleep(30);
        PostMessageW(h, WM_KEYUP, vk, 1 | (sc << 16) | (1u << 30) | (1u << 31));
        Sleep(100);
        return 0;
    }
    if (!strcmp(argv[1], "click") && argc >= 4) {
        RECT c;
        GetClientRect(h, &c);
        int fx = atoi(argv[2]), fy = atoi(argv[3]);
        /* same mapping as the app: integer scale, centred */
        int s = c.right / 720 < c.bottom / 480 ? c.right / 720 : c.bottom / 480;
        if (s < 1) s = 1;
        int ox = (c.right - 720 * s) / 2, oy = (c.bottom - 480 * s) / 2;
        LPARAM lp = MAKELPARAM(ox + fx * s + s / 2, oy + fy * s + s / 2);
        PostMessageW(h, WM_MOUSEMOVE, 0, lp);
        Sleep(50);
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        Sleep(80);
        PostMessageW(h, WM_LBUTTONUP, 0, lp);
        Sleep(100);
        return 0;
    }
    if (!strcmp(argv[1], "close")) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
    if (!strcmp(argv[1], "syskey") && argc >= 3) {   /* Alt+key */
        int vk = (int)strtol(argv[2], NULL, 0);
        int sc = (int)MapVirtualKeyW(vk, 0);
        PostMessageW(h, WM_SYSKEYDOWN, vk, 1 | (sc << 16) | (1u << 29));
        Sleep(30);
        PostMessageW(h, WM_SYSKEYUP, vk, 1 | (sc << 16) | (1u << 29) | (1u << 30) | (1u << 31));
        Sleep(100);
        return 0;
    }
    if (!strcmp(argv[1], "ctrlkey") && argc >= 3) {  /* Ctrl+key via real input state */
        int vk = (int)strtol(argv[2], NULL, 0);
        SetForegroundWindow(h);
        INPUT in[4];
        memset(in, 0, sizeof in);
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = (WORD)vk;
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = (WORD)vk; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
        Sleep(200);
        return 0;
    }
    if (!strcmp(argv[1], "dialog")) {   /* find a common dialog / message box */
        HWND d = NULL;
        for (int i = 0; i < 30 && !d; i++) { d = FindWindowW(L"#32770", NULL); if (!d) Sleep(100); }
        if (!d) { printf("no dialog\n"); return 1; }
        wchar_t t[256];
        GetWindowTextW(d, t, 256);
        printf("dialog '%ls' visible=%d\n", t, IsWindowVisible(d));
        if (argc >= 3 && !strcmp(argv[2], "cancel")) PostMessageW(d, WM_COMMAND, IDCANCEL, 0);
        return 0;
    }
    return 2;
}
