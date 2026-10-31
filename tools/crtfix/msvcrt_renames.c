/*
 * Replacement for 7 renamed msvcrt.dll imports.
 *
 * mingw-w64's msvcrt.def imports some msvcrt.dll functions under a different
 * name ("_crt_atexit == atexit", ...) because mingw-w64 provides its own
 * function with the original name. LLVM 18's llvm-dlltool does not support
 * the "==" syntax and turns these lines into weak aliases of mingw-w64's own
 * functions, so e.g. atexit() ends up calling itself forever (any program
 * hangs at startup). Those lines are removed from the import library and the
 * renamed symbols are provided here, resolving the real msvcrt.dll functions
 * at run time.
 */
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <wchar.h>

static FARPROC msvcrt_sym(const char *name)
{
    static HMODULE h;
    if (!h) h = GetModuleHandleA("msvcrt.dll");
    if (!h) h = LoadLibraryA("msvcrt.dll");
    return h ? GetProcAddress(h, name) : NULL;
}

/* _crt_atexit == atexit */
typedef int (__cdecl *atexit_fn)(void (__cdecl *)(void));
int __cdecl _crt_atexit(void (__cdecl *func)(void))
{
    static atexit_fn p;
    if (!p) p = (atexit_fn)(void *)msvcrt_sym("atexit");
    return p ? p(func) : -1;
}

/* _swprintf == swprintf (legacy msvcrt signature, no buffer size) */
typedef int (__cdecl *vswprintf_legacy_fn)(wchar_t *, const wchar_t *, va_list);
int __cdecl _vswprintf(wchar_t *buf, const wchar_t *fmt, va_list ap)
{
    static vswprintf_legacy_fn p;
    if (!p) p = (vswprintf_legacy_fn)(void *)msvcrt_sym("vswprintf");
    return p ? p(buf, fmt, ap) : -1;
}

int __cdecl _swprintf(wchar_t *buf, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vswprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

/* _wcstok == wcstok (legacy 2-argument version) */
typedef wchar_t *(__cdecl *wcstok2_fn)(wchar_t *, const wchar_t *);
wchar_t *__cdecl _wcstok(wchar_t *s, const wchar_t *delim)
{
    static wcstok2_fn p;
    if (!p) p = (wcstok2_fn)(void *)msvcrt_sym("wcstok");
    return p ? p(s, delim) : NULL;
}

/* __msvcrt_wcstok_s == wcstok_s */
typedef wchar_t *(__cdecl *wcstok_s_fn)(wchar_t *, const wchar_t *, wchar_t **);
wchar_t *__cdecl __msvcrt_wcstok_s(wchar_t *s, const wchar_t *delim, wchar_t **ctx)
{
    static wcstok_s_fn p;
    if (!p) p = (wcstok_s_fn)(void *)msvcrt_sym("wcstok_s");
    return p ? p(s, delim, ctx) : NULL;
}

/* DATA imports: mingw-w64 calls these through __imp_ pointers */
typedef void (__cdecl *assert_fn)(const char *, const char *, unsigned);
static void __cdecl assert_tramp(const char *msg, const char *file, unsigned line)
{
    assert_fn p = (assert_fn)(void *)msvcrt_sym("_assert");
    if (p) p(msg, file, line);
    else TerminateProcess(GetCurrentProcess(), 3);
}

typedef int (__cdecl *iswctype_fn)(unsigned short, unsigned short);
static int __cdecl iswctype_tramp(unsigned short c, unsigned short type)
{
    static iswctype_fn p;
    if (!p) p = (iswctype_fn)(void *)msvcrt_sym("iswctype");
    return p ? p(c, type) : 0;
}

void *__imp___msvcrt_assert = (void *)assert_tramp;
void *__imp___msvcrt_iswctype = (void *)iswctype_tramp;

/* __imp_ pointers for the function imports, in case code calls them as dllimport */
void *__imp__crt_atexit = (void *)_crt_atexit;
void *__imp__swprintf = (void *)_swprintf;
void *__imp__vswprintf = (void *)_vswprintf;
void *__imp__wcstok = (void *)_wcstok;
void *__imp___msvcrt_wcstok_s = (void *)__msvcrt_wcstok_s;
