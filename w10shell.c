/*
 * w10shell.c - Windows 10 style taskbar, Start panel and taskbar settings
 *              for Wine's explorer.exe (made for Winlator containers).
 *
 * Uses only the public Win32 API. It is linked into programs/explorer and
 * called from desktop.c (see the patch step of the GitHub workflow).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <stdarg.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#define NELEM(a)        ((int)(sizeof(a) / sizeof((a)[0])))

#define TBAR_CLASS      L"W10ShellTaskbar"
#define START_CLASS     L"W10ShellStart"
#define SETT_CLASS      L"W10ShellSettings"
#define W10_REGKEY      L"Software\\W10Shell"

enum { TID_POLL = 1, TID_ANIM = 2, TID_STABLE = 3, TID_ICONS = 4 };

#define MAX_BTNS        48
#define MAX_TRAY        32
#define MAX_PINS        48
#define POLL_MS         100

#define COL_BAR_BG      RGB(20, 20, 20)
#define COL_BAR_HOT     RGB(54, 54, 54)
#define COL_BAR_ACT     RGB(72, 72, 72)
#define COL_BAR_DOWN    RGB(38, 38, 38)
#define COL_WHITE       RGB(255, 255, 255)
#define COL_DIM         RGB(176, 176, 176)
#define COL_START_BG    RGB(31, 31, 31)
#define COL_PB_BG       RGB(24, 24, 24)
#define COL_PB_EXP      RGB(36, 36, 36)
#define COL_ROW_HOT     RGB(66, 66, 66)
#define COL_SEARCH_BG   RGB(242, 242, 242)
#define COL_SEARCH_TXT  RGB(24, 24, 24)
#define COL_FLY_BG      RGB(43, 43, 43)

typedef struct
{
    int height, autohide, reveal, reveal_delay, hide_delay, btn_width, top, accent;
} W10CFG;

static const COLORREF accent_tbl[] =
{
    RGB(0, 120, 215), RGB(116, 77, 169), RGB(0, 153, 188), RGB(16, 137, 62),
    RGB(255, 140, 0), RGB(232, 17, 35),  RGB(227, 0, 140), RGB(122, 117, 116)
};
static const WCHAR *const accent_names[] =
{
    L"Blue", L"Purple", L"Cyan", L"Green", L"Orange", L"Red", L"Pink", L"Gray"
};

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */

static HINSTANCE hinst;
static HWND tb_hwnd, sm_hwnd, st_hwnd, tray_hwnd;
static WNDPROC tray_old_proc;
static BOOL shell_on;
static HFONT f_ui, f_small, f_bold, f_title, f_bar;
static WCHAR font_face[LF_FACESIZE];
static int scr_w, scr_h;
static W10CFG cfg;
static BOOL cfg_dirty;
static BOOL in_menu;

static void sm_show(void);
static void sm_hide(void);
static void sm_toggle(void);
static BOOL sm_is_open(void);
static void open_settings(void);
static void tb_apply_cfg(BOOL final);
static void invalidate_tb(void);
static void sm_poll(void);
static void tb_slide(BOOL hide);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int iabs(int v) { return v < 0 ? -v : v; }

static void *halloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void hfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static WCHAR wupper(WCHAR c) { return (WCHAR)(ULONG_PTR)CharUpperW((LPWSTR)(ULONG_PTR)c); }
static WCHAR wlower(WCHAR c) { return (WCHAR)(ULONG_PTR)CharLowerW((LPWSTR)(ULONG_PTR)c); }

static BOOL wcontains_i(const WCHAR *hay, const WCHAR *needle)
{
    int hl = lstrlenW(hay), nl = lstrlenW(needle), i, j;
    if (!nl) return TRUE;
    for (i = 0; i + nl <= hl; i++)
    {
        for (j = 0; j < nl; j++)
            if (wlower(hay[i + j]) != wlower(needle[j])) break;
        if (j == nl) return TRUE;
    }
    return FALSE;
}

static WCHAR first_letter(const WCHAR *s)
{
    for (; *s; s++) if (IsCharAlphaNumericW(*s)) return wupper(*s);
    return L'?';
}

static const WCHAR *path_file(const WCHAR *p)
{
    const WCHAR *f = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') f = p + 1;
    return f;
}

static const WCHAR *path_ext(const WCHAR *p)
{
    const WCHAR *e = NULL;
    for (; *p; p++)
    {
        if (*p == '.') e = p;
        else if (*p == '\\' || *p == '/') e = NULL;
    }
    return e ? e : L"";
}

static void name_from_path(const WCHAR *path, WCHAR *out, int cch)
{
    const WCHAR *f = path_file(path), *e = path_ext(f);
    int n = (*e) ? (int)(e - f) : lstrlenW(f);
    if (n >= cch) n = cch - 1;
    lstrcpynW(out, f, n + 1);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (WCHAR)(out[0] - 32);
}

static BOOL file_exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static COLORREF accent_col(void) { return accent_tbl[iclamp(cfg.accent, 0, NELEM(accent_tbl) - 1)]; }

static COLORREF lighten(COLORREF c, int a)
{
    return RGB(imin(GetRValue(c) + a, 255), imin(GetGValue(c) + a, 255), imin(GetBValue(c) + a, 255));
}

static COLORREF letter_color(const WCHAR *name)
{
    static const COLORREF pal[] =
    {
        RGB(0, 120, 215), RGB(0, 153, 188), RGB(0, 178, 148), RGB(16, 137, 62),
        RGB(232, 17, 35), RGB(255, 140, 0), RGB(194, 57, 179), RGB(116, 77, 169),
        RGB(202, 80, 16), RGB(1, 133, 116), RGB(177, 70, 194), RGB(76, 74, 72)
    };
    unsigned h = 7;
    for (; *name; name++) h = h * 31 + (unsigned)*name;
    return pal[h % (unsigned)NELEM(pal)];
}

static void wlog(const char *fmt, ...)
{
    static int enabled = -1;
    char buf[1100];
    va_list ap;
    HANDLE h;
    DWORD n;

    if (enabled < 0) enabled = file_exists(L"C:\\w10shell.debug");
    if (!enabled) return;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    lstrcatA(buf, "\r\n");
    h = CreateFileW(L"C:\\w10shell.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, buf, lstrlenA(buf), &n, NULL);
        CloseHandle(h);
    }
}

/* ------------------------------------------------------------------ */
/* drawing helpers                                                     */
/* ------------------------------------------------------------------ */

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    rc.left = l; rc.top = t; rc.right = r; rc.bottom = b;
    SetDCBrushColor(dc, c);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void fill_round(HDC dc, int l, int t, int r, int b, int rad, COLORREF c)
{
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, c);
    RoundRect(dc, l, t, r + 1, b + 1, rad, rad);
    SelectObject(dc, op);
    SelectObject(dc, ob);
}

static void draw_text(HDC dc, const WCHAR *s, int x, int y, int w, int h, HFONT f, COLORREF c, UINT fmt)
{
    RECT r;
    HGDIOBJ old;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static HPEN mkpen(int w, COLORREF c) { return CreatePen(PS_SOLID, w, c); }

static HFONT badge_font(int sz)
{
    static HFONT cache[72];
    int px;
    if (sz < 8) sz = 8;
    if (sz > 71) sz = 71;
    if (!cache[sz])
    {
        px = sz * 11 / 20;
        cache[sz] = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, font_face);
    }
    return cache[sz];
}

/* coloured rounded square with one character in it */
static void draw_badge(HDC dc, int x, int y, int sz, COLORREF col, WCHAR ch)
{
    WCHAR s[2];
    s[0] = ch; s[1] = 0;
    fill_round(dc, x, y, x + sz - 1, y + sz - 1, imax(sz / 4, 4), col);
    draw_text(dc, s, x, y, sz, sz, badge_font(sz), COL_WHITE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* real icon, or the first letter of the name in a coloured round square */
static void draw_app_icon(HDC dc, int x, int y, int sz, HICON ic, const WCHAR *name)
{
    if (ic) DrawIconEx(dc, x, y, ic, sz, sz, 0, NULL, DI_NORMAL);
    else draw_badge(dc, x, y, sz, letter_color(name), first_letter(name));
}

static int CALLBACK font_enum_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    *(BOOL *)lp = TRUE;
    return 0;
}

static BOOL have_font(const WCHAR *face)
{
    LOGFONTW lf;
    HDC dc = GetDC(NULL);
    BOOL found = FALSE;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)font_enum_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static HFONT mkfont(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, font_face);
}

static void make_fonts(void)
{
    static const WCHAR *const faces[] = { L"Segoe UI", L"Tahoma", L"Arial", L"MS Shell Dlg" };
    int i;
    lstrcpyW(font_face, faces[0]);
    for (i = 0; i < NELEM(faces); i++)
        if (have_font(faces[i])) { lstrcpyW(font_face, faces[i]); break; }
    f_ui    = mkfont(15, FW_NORMAL);
    f_small = mkfont(12, FW_NORMAL);
    f_bold  = mkfont(15, FW_SEMIBOLD);
    f_title = mkfont(22, FW_LIGHT);
    f_bar   = mkfont(13, FW_NORMAL);
}

/* ------------------------------------------------------------------ */
/* registry / settings                                                 */
/* ------------------------------------------------------------------ */

static HKEY open_key(void)
{
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, W10_REGKEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS) return NULL;
    return k;
}

static DWORD reg_get(HKEY k, const WCHAR *name, DWORD def)
{
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD) return v;
    return def;
}

static void reg_set(HKEY k, const WCHAR *name, DWORD v)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
}

static void cfg_defaults(void)
{
    cfg.height = 40; cfg.autohide = 0; cfg.reveal = 4; cfg.reveal_delay = 150;
    cfg.hide_delay = 500; cfg.btn_width = 170; cfg.top = 0; cfg.accent = 0;
}

static void cfg_load(void)
{
    HKEY k = open_key();
    cfg_defaults();
    if (!k) return;
    cfg.height       = iclamp((int)reg_get(k, L"TaskbarHeight", cfg.height), 28, 96);
    cfg.autohide     = reg_get(k, L"AutoHide", cfg.autohide) ? 1 : 0;
    cfg.reveal       = iclamp((int)reg_get(k, L"RevealSize", cfg.reveal), 1, 48);
    cfg.reveal_delay = iclamp((int)reg_get(k, L"RevealDelay", cfg.reveal_delay), 0, 2000);
    cfg.hide_delay   = iclamp((int)reg_get(k, L"HideDelay", cfg.hide_delay), 0, 5000);
    cfg.btn_width    = iclamp((int)reg_get(k, L"ButtonWidth", cfg.btn_width), 90, 320);
    cfg.top          = reg_get(k, L"AtTop", cfg.top) ? 1 : 0;
    cfg.accent       = iclamp((int)reg_get(k, L"Accent", cfg.accent), 0, NELEM(accent_tbl) - 1);
    RegCloseKey(k);
}

static void cfg_save(void)
{
    HKEY k = open_key();
    if (!k) return;
    reg_set(k, L"TaskbarHeight", cfg.height);
    reg_set(k, L"AutoHide", cfg.autohide);
    reg_set(k, L"RevealSize", cfg.reveal);
    reg_set(k, L"RevealDelay", cfg.reveal_delay);
    reg_set(k, L"HideDelay", cfg.hide_delay);
    reg_set(k, L"ButtonWidth", cfg.btn_width);
    reg_set(k, L"AtTop", cfg.top);
    reg_set(k, L"Accent", cfg.accent);
    RegCloseKey(k);
    cfg_dirty = FALSE;
}

/* ------------------------------------------------------------------ */
/* icons                                                               */
/* ------------------------------------------------------------------ */

static HICON extract_ico(const WCHAR *path, int idx)
{
    HICON lg = NULL, sm = NULL;
    UINT n = ExtractIconExW(path, idx, &lg, &sm, 1);
    if (n == 0 || n == (UINT)-1) { if (lg) DestroyIcon(lg); if (sm) DestroyIcon(sm); return NULL; }
    if (lg) { if (sm) DestroyIcon(sm); return lg; }
    return sm;
}

static BOOL is_icon_container(const WCHAR *ext)
{
    static const WCHAR *const e[] = { L".exe", L".dll", L".ico", L".icl", L".cpl", L".scr", L".ocx" };
    int i;
    for (i = 0; i < NELEM(e); i++) if (!lstrcmpiW(ext, e[i])) return TRUE;
    return FALSE;
}

/* icon of a plain file/folder; NULL when it has no icon of its own */
static HICON icon_of_target(const WCHAR *path)
{
    DWORD a;
    SHFILEINFOW sfi;
    if (!path[0]) return NULL;
    if (is_icon_container(path_ext(path))) return extract_ico(path, 0);
    a = GetFileAttributesW(path);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
    {
        ZeroMemory(&sfi, sizeof(sfi));
        if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)) return sfi.hIcon;
    }
    return NULL;
}

static HICON icon_of_lnk(const WCHAR *lnk)
{
    IShellLinkW *psl = NULL;
    IPersistFile *ppf = NULL;
    WCHAR target[MAX_PATH], iconp[MAX_PATH], ex[MAX_PATH];
    int idx = 0;
    HICON ic = NULL;

    target[0] = iconp[0] = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&psl)))
    {
        if (SUCCEEDED(IShellLinkW_QueryInterface(psl, &IID_IPersistFile, (void **)&ppf)))
        {
            if (SUCCEEDED(IPersistFile_Load(ppf, lnk, STGM_READ)))
            {
                IShellLinkW_GetPath(psl, target, MAX_PATH, NULL, 0);
                IShellLinkW_GetIconLocation(psl, iconp, MAX_PATH, &idx);
            }
            IPersistFile_Release(ppf);
        }
        IShellLinkW_Release(psl);
    }
    if (iconp[0])
    {
        ExpandEnvironmentStringsW(iconp, ex, MAX_PATH);
        ic = extract_ico(ex, idx);
    }
    if (!ic && target[0])
    {
        ExpandEnvironmentStringsW(target, ex, MAX_PATH);
        ic = icon_of_target(ex);
    }
    return ic;
}

static HICON icon_for_file(const WCHAR *path)
{
    if (!lstrcmpiW(path_ext(path), L".lnk")) return icon_of_lnk(path);
    return icon_of_target(path);
}

/* ------------------------------------------------------------------ */
/* Start menu application list                                         */
/* ------------------------------------------------------------------ */

typedef struct { WCHAR name[96]; WCHAR path[MAX_PATH]; HICON icon; BYTE loaded; } W10APP;
typedef struct { short type; short letter; int app; } W10ROW;     /* 0 header, 1 app, 2 run-command */
typedef struct { WCHAR path[MAX_PATH]; WCHAR name[64]; HICON icon; BYTE loaded; } W10PIN;

static W10APP *apps;
static int app_cnt, app_cap;
static int *sorted, sorted_cnt;
static W10ROW *rows;
static int row_cnt, list_h;
static DWORD last_scan;
static W10PIN pins[MAX_PINS];
static int pin_cnt;
static WCHAR query[128];
static int qlen;
static int list_scroll, tile_scroll;

static void app_add(const WCHAR *path, const WCHAR *name)
{
    int i;
    W10APP *n;
    for (i = 0; i < app_cnt; i++) if (!lstrcmpiW(apps[i].name, name)) return;
    if (app_cnt == app_cap)
    {
        int nc = app_cap ? app_cap * 2 : 128;
        n = apps ? (W10APP *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, apps, nc * sizeof(W10APP))
                 : (W10APP *)halloc(nc * sizeof(W10APP));
        if (!n) return;
        apps = n;
        app_cap = nc;
    }
    ZeroMemory(&apps[app_cnt], sizeof(W10APP));
    lstrcpynW(apps[app_cnt].name, name, 96);
    lstrcpynW(apps[app_cnt].path, path, MAX_PATH);
    app_cnt++;
}

static void scan_dir(const WCHAR *dir, int depth)
{
    WCHAR pat[MAX_PATH], full[MAX_PATH], nm[96];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (lstrlenW(dir) > MAX_PATH - 8) return;
    lstrcpyW(pat, dir);
    lstrcatW(pat, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
        if (lstrlenW(dir) + lstrlenW(fd.cFileName) + 2 >= MAX_PATH) continue;
        lstrcpyW(full, dir);
        lstrcatW(full, L"\\");
        lstrcatW(full, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth < 5) scan_dir(full, depth + 1);
            continue;
        }
        if (lstrcmpiW(path_ext(fd.cFileName), L".lnk")) continue;
        name_from_path(fd.cFileName, nm, 96);
        if (wcontains_i(nm, L"uninstall")) continue;
        app_add(full, nm);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int sort_cmp(int a, int b) { return lstrcmpiW(apps[a].name, apps[b].name); }

static void sort_apps(void)
{
    int i, j, t;
    hfree(sorted);
    sorted = (int *)halloc((app_cnt + 1) * sizeof(int));
    sorted_cnt = sorted ? app_cnt : 0;
    for (i = 0; i < sorted_cnt; i++) sorted[i] = i;
    for (i = 1; i < sorted_cnt; i++)
    {
        t = sorted[i];
        for (j = i - 1; j >= 0 && sort_cmp(sorted[j], t) > 0; j--) sorted[j + 1] = sorted[j];
        sorted[j + 1] = t;
    }
}

static WCHAR group_of(const WCHAR *name)
{
    WCHAR c = wupper(name[0]);
    return (c >= 'A' && c <= 'Z') ? c : L'#';
}

static void rebuild_rows(void)
{
    int i;
    WCHAR g = 0;

    hfree(rows);
    rows = (W10ROW *)halloc((sorted_cnt * 2 + 8) * sizeof(W10ROW));
    row_cnt = 0;
    list_h = 0;
    if (!rows) return;
    for (i = 0; i < sorted_cnt; i++)
    {
        W10APP *a = &apps[sorted[i]];
        if (qlen)
        {
            if (!wcontains_i(a->name, query)) continue;
        }
        else
        {
            WCHAR gg = group_of(a->name);
            if (gg != g)
            {
                g = gg;
                rows[row_cnt].type = 0; rows[row_cnt].letter = (short)g; rows[row_cnt].app = -1;
                row_cnt++;
                list_h += 28;
            }
        }
        rows[row_cnt].type = 1; rows[row_cnt].letter = 0; rows[row_cnt].app = sorted[i];
        row_cnt++;
        list_h += 40;
    }
    if (qlen)
    {
        rows[row_cnt].type = 2; rows[row_cnt].letter = 0; rows[row_cnt].app = -1;
        row_cnt++;
        list_h += 40;
    }
}

static void apps_rescan(void)
{
    W10APP *old = apps;
    int old_cnt = app_cnt, i, j;
    WCHAR dir[MAX_PATH];

    apps = NULL; app_cnt = 0; app_cap = 0;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, dir))) scan_dir(dir, 0);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, dir))) scan_dir(dir, 0);

    /* keep icons we already extracted */
    for (i = 0; i < app_cnt; i++)
        for (j = 0; j < old_cnt; j++)
            if (old[j].loaded && !lstrcmpiW(old[j].path, apps[i].path))
            {
                apps[i].icon = old[j].icon;
                apps[i].loaded = 1;
                old[j].icon = NULL;
                break;
            }
    for (j = 0; j < old_cnt; j++) if (old[j].icon) DestroyIcon(old[j].icon);
    hfree(old);
    sort_apps();
    last_scan = GetTickCount();
}

/* ------------------------------------------------------------------ */
/* pinned tiles                                                        */
/* ------------------------------------------------------------------ */

static int pin_find(const WCHAR *path)
{
    int i;
    for (i = 0; i < pin_cnt; i++) if (!lstrcmpiW(pins[i].path, path)) return i;
    return -1;
}

static BOOL pin_add(const WCHAR *path, const WCHAR *name)
{
    if (pin_cnt >= MAX_PINS || pin_find(path) >= 0) return FALSE;
    ZeroMemory(&pins[pin_cnt], sizeof(W10PIN));
    lstrcpynW(pins[pin_cnt].path, path, MAX_PATH);
    if (name && *name) lstrcpynW(pins[pin_cnt].name, name, 64);
    else name_from_path(path, pins[pin_cnt].name, 64);
    pin_cnt++;
    return TRUE;
}

static void pin_remove(int i)
{
    if (i < 0 || i >= pin_cnt) return;
    if (pins[i].icon) DestroyIcon(pins[i].icon);
    MoveMemory(&pins[i], &pins[i + 1], (pin_cnt - i - 1) * sizeof(W10PIN));
    pin_cnt--;
    ZeroMemory(&pins[pin_cnt], sizeof(W10PIN));
}

static void pins_save(void)
{
    HKEY k = open_key();
    WCHAR *buf;
    int i, pos = 0;
    SIZE_T cap;

    if (!k) return;
    cap = (SIZE_T)pin_cnt * (MAX_PATH + 80) + 4;
    buf = (WCHAR *)halloc(cap * sizeof(WCHAR));
    if (buf)
    {
        for (i = 0; i < pin_cnt; i++)
        {
            lstrcpyW(buf + pos, pins[i].path);
            pos += lstrlenW(pins[i].path);
            buf[pos++] = '|';
            lstrcpyW(buf + pos, pins[i].name);
            pos += lstrlenW(pins[i].name) + 1;
        }
        buf[pos++] = 0;
        RegSetValueExW(k, L"Pinned", 0, REG_MULTI_SZ, (const BYTE *)buf, pos * sizeof(WCHAR));
        hfree(buf);
        reg_set(k, L"PinnedInit", 1);
    }
    RegCloseKey(k);
}

static void seed_one(const WCHAR *file, const WCHAR *name)
{
    WCHAR root[3][MAX_PATH], p[MAX_PATH];
    int i;
    GetSystemDirectoryW(root[0], MAX_PATH);
    GetWindowsDirectoryW(root[1], MAX_PATH);
    root[2][0] = 0;
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT, root[2]);
    for (i = 0; i < 3; i++)
    {
        if (!root[i][0] || lstrlenW(root[i]) + lstrlenW(file) + 2 >= MAX_PATH) continue;
        lstrcpyW(p, root[i]);
        lstrcatW(p, L"\\");
        lstrcatW(p, file);
        if (file_exists(p)) { pin_add(p, name); return; }
    }
}

static void seed_pins(void)
{
    seed_one(L"explorer.exe", L"File Explorer");
    seed_one(L"notepad.exe", L"Notepad");
    seed_one(L"Windows NT\\Accessories\\wordpad.exe", L"WordPad");
    seed_one(L"cmd.exe", L"Command Prompt");
    seed_one(L"taskmgr.exe", L"Task Manager");
    seed_one(L"regedit.exe", L"Registry Editor");
    seed_one(L"control.exe", L"Control Panel");
    seed_one(L"winecfg.exe", L"Wine Config");
    seed_one(L"winefile.exe", L"File Manager");
}

static void pins_load(void)
{
    HKEY k = open_key();
    DWORD type = 0, sz = 0;
    WCHAR *buf, *p, *bar;

    pin_cnt = 0;
    if (!k) { seed_pins(); return; }
    if (!reg_get(k, L"PinnedInit", 0))
    {
        RegCloseKey(k);
        seed_pins();
        pins_save();
        return;
    }
    if (RegQueryValueExW(k, L"Pinned", NULL, &type, NULL, &sz) == ERROR_SUCCESS && type == REG_MULTI_SZ && sz >= 2)
    {
        buf = (WCHAR *)halloc(sz + 8);
        if (buf)
        {
            DWORD sz2 = sz;
            if (RegQueryValueExW(k, L"Pinned", NULL, &type, (BYTE *)buf, &sz2) == ERROR_SUCCESS)
            {
                for (p = buf; *p; p += lstrlenW(p) + 1)
                {
                    for (bar = p; *bar && *bar != '|'; bar++) ;
                    if (*bar == '|') { *bar = 0; pin_add(p, bar + 1); *bar = '|'; }
                    else pin_add(p, NULL);
                }
            }
            hfree(buf);
        }
    }
    RegCloseKey(k);
}

/* load a few icons per call; returns TRUE while work remains */
static BOOL load_some_icons(int budget)
{
    int i;
    for (i = 0; i < pin_cnt; i++)
    {
        if (pins[i].loaded) continue;
        if (budget-- <= 0) return TRUE;
        pins[i].icon = icon_for_file(pins[i].path);
        pins[i].loaded = 1;
    }
    for (i = 0; i < row_cnt; i++)
    {
        W10APP *a;
        if (rows[i].type != 1) continue;
        a = &apps[rows[i].app];
        if (a->loaded) continue;
        if (budget-- <= 0) return TRUE;
        a->icon = icon_for_file(a->path);
        a->loaded = 1;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* system tray (passive listener on Wine's own Shell_TrayWnd)          */
/* ------------------------------------------------------------------ */

typedef struct
{
    HWND owner; UINT id; UINT cb; UINT version; HICON icon; BOOL hidden; WCHAR tip[128];
} W10TRAY;

/* layout of the platform independent NOTIFYICONDATA that shell32 sends to explorer */
typedef struct
{
    LONG  hwnd; UINT id, flags, cb_msg;
    WCHAR tip[128];
    DWORD state, state_mask;
    WCHAR info[256];
    UINT  to_ver;
    WCHAR info_title[64];
    DWORD info_flags;
    GUID  guid;
} W10NHDR;

typedef struct { W10NHDR h; UINT width, height, planes, bpp; } W10NCLS;

static W10TRAY tray[MAX_TRAY];
static int tray_cnt;
static int tray_vis[MAX_TRAY], tray_vis_cnt;

static int tray_find(HWND owner, UINT id)
{
    int i;
    for (i = 0; i < tray_cnt; i++) if (tray[i].owner == owner && tray[i].id == id) return i;
    return -1;
}

static void tray_remove(int i)
{
    if (i < 0 || i >= tray_cnt) return;
    if (tray[i].icon) DestroyIcon(tray[i].icon);
    MoveMemory(&tray[i], &tray[i + 1], (tray_cnt - i - 1) * sizeof(W10TRAY));
    tray_cnt--;
    ZeroMemory(&tray[tray_cnt], sizeof(W10TRAY));
}

static HICON icon_from_bits(const W10NCLS *c, DWORD cb)
{
    const BYTE *bits = (const BYTE *)(c + 1);
    UINT w = c->width, h = c->height, pl = c->planes, bpp = c->bpp;
    DWORD mask_sz, col_sz;
    HBITMAP bm, bc;
    ICONINFO ii;
    HICON ic;

    if (!w || !h || w > 256 || h > 256 || !pl || pl > 4 || !bpp || bpp > 32) return NULL;
    mask_sz = ((w + 15) / 16 * 2) * h;
    col_sz = ((w * bpp + 15) / 16 * 2) * h * pl;
    if (cb < sizeof(*c) + mask_sz + col_sz) return NULL;
    bm = CreateBitmap(w, h, 1, 1, bits);
    bc = CreateBitmap(w, h, pl, bpp, bits + mask_sz);
    if (!bm || !bc)
    {
        if (bm) DeleteObject(bm);
        if (bc) DeleteObject(bc);
        return NULL;
    }
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = bm;
    ii.hbmColor = bc;
    ic = CreateIconIndirect(&ii);
    DeleteObject(bm);
    DeleteObject(bc);
    return ic;
}

/* Looks at a copy of what an application sent to the tray. Never touches Wine's own handling. */
static void tray_parse(const COPYDATASTRUCT *cds)
{
    const W10NHDR *h;
    DWORD cb = cds->cbData;
    HICON ic = NULL;
    HWND owner;
    int i;

    if (!cds->lpData || cds->dwData > NIM_SETVERSION) return;
    if (cb < sizeof(W10NHDR))
    {
        wlog("tray: msg %lu cbData %lu too small, ignored", (unsigned long)cds->dwData, (unsigned long)cb);
        return;
    }
    h = (const W10NHDR *)cds->lpData;
    owner = (HWND)(LONG_PTR)h->hwnd;
    if (!IsWindow(owner)) return;
    i = tray_find(owner, h->id);

    switch (cds->dwData)
    {
    case NIM_DELETE:
        if (i >= 0) tray_remove(i);
        break;

    case NIM_ADD:
    case NIM_MODIFY:
        if (h->flags & NIF_ICON)
        {
            if (cb >= sizeof(W10NCLS)) ic = icon_from_bits((const W10NCLS *)h, cb);
            else if (cb >= sizeof(W10NHDR) + sizeof(LONG))
            {
                LONG hi = *(const LONG *)((const BYTE *)h + sizeof(W10NHDR));
                if (hi) ic = CopyIcon((HICON)(LONG_PTR)hi);
            }
            if (!ic) wlog("tray: icon not decoded, cbData %lu", (unsigned long)cb);
        }
        if (i < 0)
        {
            if (tray_cnt >= MAX_TRAY) { if (ic) DestroyIcon(ic); return; }
            i = tray_cnt++;
            ZeroMemory(&tray[i], sizeof(W10TRAY));
            tray[i].owner = owner;
            tray[i].id = h->id;
        }
        if (h->flags & NIF_MESSAGE) tray[i].cb = h->cb_msg;
        if (h->flags & NIF_TIP) lstrcpynW(tray[i].tip, h->tip, 128);
        if (h->state_mask & NIS_HIDDEN) tray[i].hidden = (h->state & NIS_HIDDEN) != 0;
        if (ic)
        {
            if (tray[i].icon) DestroyIcon(tray[i].icon);
            tray[i].icon = ic;
        }
        break;

    case NIM_SETVERSION:
        if (i >= 0) tray[i].version = h->to_ver;
        break;
    }
    invalidate_tb();
}

static LRESULT CALLBACK tray_sub_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_COPYDATA:
    {
        LRESULT r = CallWindowProcW(tray_old_proc, hwnd, msg, wp, lp);
        if (lp) tray_parse((const COPYDATASTRUCT *)lp);
        return r;
    }
    case WM_WINDOWPOSCHANGING:
        if (lp)
        {
            WINDOWPOS *wpos = (WINDOWPOS *)lp;
            wpos->flags &= ~SWP_SHOWWINDOW;
            wpos->flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
        }
        break;
    }
    return CallWindowProcW(tray_old_proc, hwnd, msg, wp, lp);
}

static void tray_notify(const W10TRAY *t, UINT ev)
{
    POINT pt;
    if (!t->cb || !IsWindow(t->owner)) return;
    GetCursorPos(&pt);
    AllowSetForegroundWindow(ASFW_ANY);
    if (t->version >= 4)
        PostMessageW(t->owner, t->cb, MAKEWPARAM(pt.x, pt.y), MAKELPARAM(ev, t->id));
    else
        PostMessageW(t->owner, t->cb, (WPARAM)t->id, (LPARAM)ev);
}

static BOOL tray_prune(void)
{
    int i;
    BOOL ch = FALSE;
    for (i = tray_cnt - 1; i >= 0; i--)
        if (!IsWindow(tray[i].owner)) { tray_remove(i); ch = TRUE; }
    return ch;
}

/* ------------------------------------------------------------------ */
/* taskbar                                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    HWND hwnd; WCHAR title[96]; HICON icon; BOOL own; BOOL seen; BOOL minimized;
    DWORD itick; int fit_tries;
} W10BTN;

static W10BTN btns[MAX_BTNS];
static int btn_cnt;
static RECT r_start, r_area, r_tray, r_clock, r_desk;
static int btn_w_cur, tray_cell, tray_isz;
static int hot_part, hot_idx, down_part, down_idx, drag_x0, drag_off;
static BOOL dragging, tracking;
static WCHAR clock_time[32], clock_date[32];
static int clock_w = 92;
static HWND active_win;
static int cur_y, tgt_y;
static BOOL tb_hidden, ah_hidden, fs_hidden;
static DWORD reveal_t, hide_t;
static BOOL reveal_pend, hide_pend;
static HWND desk_hidden[MAX_BTNS];
static int desk_cnt;
static BOOL desk_active;

static void invalidate_tb(void) { if (tb_hwnd) InvalidateRect(tb_hwnd, NULL, FALSE); }

static HICON win_icon(HWND h, BOOL *own, BOOL allow_exe)
{
    DWORD_PTR r = 0;
    HICON ic = NULL;
    *own = FALSE;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &r) && r) ic = (HICON)r;
    if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 40, &r) && r) ic = (HICON)r;
    if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICON);
    if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
    if (!ic && allow_exe)
    {
        DWORD pid = 0, len = MAX_PATH;
        WCHAR path[MAX_PATH];
        HANDLE hp;
        GetWindowThreadProcessId(h, &pid);
        hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp)
        {
            if (QueryFullProcessImageNameW(hp, 0, path, &len)) { ic = extract_ico(path, 0); *own = (ic != NULL); }
            CloseHandle(hp);
        }
    }
    return ic;
}

static BOOL CALLBACK enum_win_cb(HWND h, LPARAM lp)
{
    LONG_PTR ex;
    WCHAR title[96];
    DWORD pid = 0;
    int i;
    BOOL *changed = (BOOL *)lp;
    BOOL mini;

    if (!IsWindowVisible(h)) return TRUE;
    ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return TRUE;
    if ((ex & WS_EX_NOACTIVATE) && !(ex & WS_EX_APPWINDOW)) return TRUE;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && h != st_hwnd) return TRUE;
    if (!GetWindowTextW(h, title, NELEM(title))) return TRUE;

    for (i = 0; i < btn_cnt; i++) if (btns[i].hwnd == h) break;
    if (i == btn_cnt)
    {
        if (btn_cnt >= MAX_BTNS) return TRUE;
        ZeroMemory(&btns[i], sizeof(W10BTN));
        btns[i].hwnd = h;
        btn_cnt++;
        *changed = TRUE;
    }
    btns[i].seen = TRUE;
    if (lstrcmpW(btns[i].title, title)) { lstrcpyW(btns[i].title, title); *changed = TRUE; }
    mini = IsIconic(h);
    if (mini != btns[i].minimized) { btns[i].minimized = mini; *changed = TRUE; }
    if (!btns[i].icon || GetTickCount() - btns[i].itick > 4000)
    {
        BOOL own = FALSE;
        HICON ic = win_icon(h, &own, !btns[i].icon);
        btns[i].itick = GetTickCount();
        if (ic && ic != btns[i].icon)
        {
            if (btns[i].own && btns[i].icon) DestroyIcon(btns[i].icon);
            btns[i].icon = ic;
            btns[i].own = own;
            *changed = TRUE;
        }
    }
    return TRUE;
}

static BOOL tb_sync_windows(void)
{
    BOOL changed = FALSE;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : NULL;
    int i;

    for (i = 0; i < btn_cnt; i++) btns[i].seen = FALSE;
    EnumWindows(enum_win_cb, (LPARAM)&changed);
    for (i = btn_cnt - 1; i >= 0; i--)
    {
        if (btns[i].seen) continue;
        if (btns[i].own && btns[i].icon) DestroyIcon(btns[i].icon);
        MoveMemory(&btns[i], &btns[i + 1], (btn_cnt - i - 1) * sizeof(W10BTN));
        btn_cnt--;
        ZeroMemory(&btns[btn_cnt], sizeof(W10BTN));
        changed = TRUE;
    }
    if (root != active_win) { active_win = root; changed = TRUE; }
    return changed;
}

static BOOL update_clock(void)
{
    SYSTEMTIME st;
    WCHAR t[32], d[32];
    HDC dc;
    HGDIOBJ of;
    SIZE s1, s2;

    GetLocalTime(&st);
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, t, 32);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 32);
    if (!lstrcmpW(t, clock_time) && !lstrcmpW(d, clock_date)) return FALSE;
    lstrcpyW(clock_time, t);
    lstrcpyW(clock_date, d);
    dc = GetDC(NULL);
    of = SelectObject(dc, f_bar);
    GetTextExtentPoint32W(dc, t, lstrlenW(t), &s1);
    GetTextExtentPoint32W(dc, d, lstrlenW(d), &s2);
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);
    clock_w = imax(s1.cx, s2.cx) + 28;
    return TRUE;
}

static int tray_cell_left(int k) { return r_clock.left - (tray_vis_cnt - k) * tray_cell; }

static void tb_layout(int W, int H)
{
    int startw = imax(H * 6 / 5, 40), deskw = 10, i;

    tray_isz = iclamp(H * 2 / 5, 16, 32);
    tray_cell = tray_isz + 12;
    tray_vis_cnt = 0;
    for (i = 0; i < tray_cnt; i++) if (!tray[i].hidden) tray_vis[tray_vis_cnt++] = i;
    SetRect(&r_start, 0, 0, startw, H);
    SetRect(&r_desk, W - deskw, 0, W, H);
    SetRect(&r_clock, r_desk.left - clock_w, 0, r_desk.left, H);
    SetRect(&r_tray, r_clock.left - tray_vis_cnt * tray_cell - (tray_vis_cnt ? 4 : 0), 0, r_clock.left, H);
    SetRect(&r_area, startw, 0, r_tray.left - 6, H);
    btn_w_cur = cfg.btn_width;
    if (btn_cnt > 0 && btn_cnt * btn_w_cur > r_area.right - r_area.left)
        btn_w_cur = imax((r_area.right - r_area.left) / btn_cnt, 44);
}

static BOOL btn_rect(int i, RECT *rc)
{
    int x = r_area.left + i * btn_w_cur;
    if (i < 0 || i >= btn_cnt || x + 20 > r_area.right) return FALSE;
    rc->left = x + 1;
    rc->right = imin(x + btn_w_cur, r_area.right) - 1;
    rc->top = r_area.top;
    rc->bottom = r_area.bottom;
    return TRUE;
}

static void tb_hit(int x, int y, int *part, int *idx)
{
    POINT pt;
    RECT rc;
    pt.x = x; pt.y = y;
    *part = 0; *idx = -1;
    if (PtInRect(&r_start, pt)) { *part = 1; return; }
    if (PtInRect(&r_desk, pt)) { *part = 5; return; }
    if (PtInRect(&r_clock, pt)) { *part = 4; return; }
    if (tray_vis_cnt && PtInRect(&r_tray, pt))
    {
        int k = (x - tray_cell_left(0)) / tray_cell;
        if (x >= tray_cell_left(0) && k >= 0 && k < tray_vis_cnt) { *part = 3; *idx = k; }
        return;
    }
    if (PtInRect(&r_area, pt) && btn_w_cur > 0)
    {
        int i = (x - r_area.left) / btn_w_cur;
        if (btn_rect(i, &rc)) { *part = 2; *idx = i; }
    }
}

static void draw_logo(HDC dc, const RECT *r)
{
    int s = imax(14, (r->bottom - r->top) * 2 / 5), pane, x, y;
    pane = (s - 1) / 2;
    x = (r->left + r->right - (pane * 2 + 1)) / 2;
    y = (r->top + r->bottom - (pane * 2 + 1)) / 2;
    fill(dc, x, y, x + pane, y + pane, COL_WHITE);
    fill(dc, x + pane + 1, y, x + pane * 2 + 1, y + pane, COL_WHITE);
    fill(dc, x, y + pane + 1, x + pane, y + pane * 2 + 1, COL_WHITE);
    fill(dc, x + pane + 1, y + pane + 1, x + pane * 2 + 1, y + pane * 2 + 1, COL_WHITE);
}

static void draw_btn(HDC dc, int i, const RECT *rc, BOOL lifted)
{
    W10BTN *b = &btns[i];
    BOOL act = (b->hwnd == active_win) && !b->minimized;
    BOOL hot = (hot_part == 2 && hot_idx == i) || lifted;
    BOOL down = (down_part == 2 && down_idx == i) && !dragging;
    COLORREF bg, ul;
    int isz = iclamp(cfg.height * 3 / 5, 16, 40), w = rc->right - rc->left, h = rc->bottom - rc->top, ix, iy;

    if (act) bg = hot ? lighten(COL_BAR_ACT, 14) : COL_BAR_ACT;
    else bg = down ? COL_BAR_DOWN : (hot ? COL_BAR_HOT : COL_BAR_BG);
    fill(dc, rc->left, rc->top, rc->right, rc->bottom, bg);
    iy = rc->top + (h - isz) / 2;
    ix = (w >= 90) ? rc->left + 10 : rc->left + (w - isz) / 2;
    draw_app_icon(dc, ix, iy, isz, b->icon, b->title);
    if (w >= 90)
        draw_text(dc, b->title, ix + isz + 8, rc->top, rc->right - (ix + isz + 8) - 8, h, f_bar,
                  b->minimized ? COL_DIM : COL_WHITE, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ul = act ? lighten(accent_col(), 70) : (b->minimized ? RGB(96, 96, 96) : RGB(150, 150, 150));
    fill(dc, rc->left, rc->bottom - 2, rc->right, rc->bottom, ul);
}

static void tb_paint(HDC dc, int W, int H)
{
    int i, k;
    RECT rc;

    fill(dc, 0, 0, W, H, COL_BAR_BG);
    if (tb_hidden)
    {
        if (cfg.top) fill(dc, 0, H - cfg.reveal, W, H, accent_col());
        else fill(dc, 0, 0, W, cfg.reveal, accent_col());
        return;
    }
    tb_layout(W, H);

    /* start button */
    if (sm_is_open()) fill(dc, r_start.left, 0, r_start.right, H, COL_BAR_ACT);
    else if (hot_part == 1) fill(dc, r_start.left, 0, r_start.right, H, (down_part == 1) ? COL_BAR_DOWN : COL_BAR_HOT);
    draw_logo(dc, &r_start);

    /* running applications */
    SaveDC(dc);
    IntersectClipRect(dc, r_area.left, 0, r_area.right, H);
    for (i = 0; i < btn_cnt; i++)
    {
        if (dragging && down_part == 2 && i == down_idx) continue;
        if (btn_rect(i, &rc)) draw_btn(dc, i, &rc, FALSE);
    }
    if (dragging && down_part == 2 && btn_rect(down_idx, &rc))
    {
        POINT pt;
        int w = rc.right - rc.left;
        GetCursorPos(&pt);
        ScreenToClient(tb_hwnd, &pt);
        rc.left = iclamp(pt.x - drag_off, r_area.left, imax(r_area.right - w, r_area.left));
        rc.right = rc.left + w;
        draw_btn(dc, down_idx, &rc, TRUE);
    }
    RestoreDC(dc, -1);

    /* tray */
    for (k = 0; k < tray_vis_cnt; k++)
    {
        W10TRAY *t = &tray[tray_vis[k]];
        int cx = tray_cell_left(k);
        if (hot_part == 3 && hot_idx == k) fill(dc, cx, 0, cx + tray_cell, H, COL_BAR_HOT);
        draw_app_icon(dc, cx + (tray_cell - tray_isz) / 2, (H - tray_isz) / 2, tray_isz, t->icon, t->tip[0] ? t->tip : L"?");
    }

    /* clock: time on top, date below */
    if (hot_part == 4) fill(dc, r_clock.left, 0, r_clock.right, H, COL_BAR_HOT);
    draw_text(dc, clock_time, r_clock.left, 0, r_clock.right - r_clock.left, H / 2, f_bar, COL_WHITE,
              DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    draw_text(dc, clock_date, r_clock.left, H / 2, r_clock.right - r_clock.left, H - H / 2, f_bar, COL_WHITE,
              DT_CENTER | DT_TOP | DT_SINGLELINE);

    /* show desktop */
    if (hot_part == 5) fill(dc, r_desk.left, 0, r_desk.right, H, COL_BAR_HOT);
    fill(dc, r_desk.left, 0, r_desk.left + 1, H, RGB(120, 120, 120));
}

/* ------------------------------------------------------------------ */
/* taskbar behaviour                                                   */
/* ------------------------------------------------------------------ */

static void tb_workarea(RECT *wa)
{
    wa->left = 0; wa->top = 0; wa->right = scr_w; wa->bottom = scr_h;
    if (!cfg.autohide)
    {
        if (cfg.top) wa->top = cfg.height;
        else wa->bottom = scr_h - cfg.height;
    }
}

static void tb_apply_workarea(void)
{
    RECT wa;
    tb_workarea(&wa);
    SystemParametersInfoW(SPI_SETWORKAREA, 0, &wa, SPIF_SENDCHANGE);
}

static void tb_apply_cfg(BOOL final)
{
    int shown_y;
    scr_w = GetSystemMetrics(SM_CXSCREEN);
    scr_h = GetSystemMetrics(SM_CYSCREEN);
    shown_y = cfg.top ? 0 : scr_h - cfg.height;
    KillTimer(tb_hwnd, TID_ANIM);
    ah_hidden = FALSE;
    tb_hidden = FALSE;
    cur_y = tgt_y = shown_y;
    SetWindowPos(tb_hwnd, HWND_TOPMOST, 0, cur_y, scr_w, cfg.height,
                 SWP_NOACTIVATE | (fs_hidden ? 0 : SWP_SHOWWINDOW));
    InvalidateRect(tb_hwnd, NULL, FALSE);
    if (final) tb_apply_workarea();
}

static void tb_slide(BOOL hide)
{
    int shown_y = cfg.top ? 0 : scr_h - cfg.height;
    int hidden_y = cfg.top ? -(cfg.height - cfg.reveal) : scr_h - cfg.reveal;
    ah_hidden = hide;
    if (!hide) { tb_hidden = FALSE; invalidate_tb(); }
    tgt_y = hide ? hidden_y : shown_y;
    SetTimer(tb_hwnd, TID_ANIM, 16, NULL);
}

static void tb_anim(void)
{
    int d = tgt_y - cur_y, step;
    if (d)
    {
        step = d / 3;
        if (!step) step = d > 0 ? 1 : -1;
        cur_y += step;
        SetWindowPos(tb_hwnd, HWND_TOPMOST, 0, cur_y, scr_w, cfg.height, SWP_NOACTIVATE);
    }
    if (cur_y == tgt_y)
    {
        KillTimer(tb_hwnd, TID_ANIM);
        tb_hidden = ah_hidden && cfg.autohide;
        invalidate_tb();
    }
}

static BOOL cursor_in_zone(void)
{
    POINT pt;
    int sz = ah_hidden ? cfg.reveal : cfg.height;
    GetCursorPos(&pt);
    if (cfg.top) return pt.y < sz;
    return pt.y >= scr_h - sz;
}

static void tb_autohide_step(void)
{
    DWORD now = GetTickCount();
    BOOL inside;

    if (!cfg.autohide)
    {
        if (ah_hidden) tb_slide(FALSE);
        return;
    }
    inside = cursor_in_zone() || sm_is_open() || in_menu || dragging;
    if (ah_hidden)
    {
        hide_pend = FALSE;
        if (inside)
        {
            if (!reveal_pend) { reveal_pend = TRUE; reveal_t = now; }
            if (now - reveal_t >= (DWORD)cfg.reveal_delay) { reveal_pend = FALSE; tb_slide(FALSE); }
        }
        else reveal_pend = FALSE;
    }
    else
    {
        reveal_pend = FALSE;
        if (!inside)
        {
            if (!hide_pend) { hide_pend = TRUE; hide_t = now; }
            if (now - hide_t >= (DWORD)cfg.hide_delay) { hide_pend = FALSE; tb_slide(TRUE); }
        }
        else hide_pend = FALSE;
    }
}

static BOOL fullscreen_fg(void)
{
    HWND fg = GetForegroundWindow();
    RECT r;
    DWORD pid = 0;
    LONG_PTR st;

    if (!fg || fg == tb_hwnd || fg == sm_hwnd || fg == st_hwnd || fg == GetDesktopWindow()) return FALSE;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return FALSE;
    if (!IsWindowVisible(fg) || IsIconic(fg)) return FALSE;
    st = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((st & WS_CAPTION) == WS_CAPTION) return FALSE;
    GetWindowRect(fg, &r);
    return r.left <= 0 && r.top <= 0 && r.right >= scr_w && r.bottom >= scr_h;
}

/* if the work area was not honoured, pull maximised windows out from under the bar */
static void tb_fit_zoomed(void)
{
    RECT wa, r;
    int i, slack = GetSystemMetrics(SM_CYFRAME) + 12;

    if (cfg.autohide || fs_hidden) return;
    tb_workarea(&wa);
    for (i = 0; i < btn_cnt; i++)
    {
        HWND h = btns[i].hwnd;
        LONG_PTR st;
        if (!IsZoomed(h)) { btns[i].fit_tries = 0; continue; }
        st = GetWindowLongPtrW(h, GWL_STYLE);
        if ((st & WS_CAPTION) != WS_CAPTION || btns[i].fit_tries >= 3) continue;
        GetWindowRect(h, &r);
        if (!cfg.top && r.bottom > wa.bottom + slack)
        {
            btns[i].fit_tries++;
            SetWindowPos(h, NULL, r.left, r.top, r.right - r.left,
                         wa.bottom + GetSystemMetrics(SM_CYFRAME) - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else if (cfg.top && r.top < wa.top - slack)
        {
            int nt = wa.top - GetSystemMetrics(SM_CYFRAME);
            btns[i].fit_tries++;
            SetWindowPos(h, NULL, r.left, nt, r.right - r.left, r.bottom - nt, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

static void tb_poll(void)
{
    static int tick;
    BOOL ch = FALSE;

    tick++;
    tb_autohide_step();
    sm_poll();
    if (tick % 4) return;

    {
        BOOL fs = fullscreen_fg();
        if (fs != fs_hidden)
        {
            fs_hidden = fs;
            if (fs) { sm_hide(); ShowWindow(tb_hwnd, SW_HIDE); }
            else SetWindowPos(tb_hwnd, HWND_TOPMOST, 0, cur_y, scr_w, cfg.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
    if (fs_hidden) return;
    ch |= tb_sync_windows();
    ch |= tray_prune();
    ch |= update_clock();
    if (tick % 12 == 0) tb_fit_zoomed();
    if (cfg_dirty && tick % 8 == 0) cfg_save();
    if (ch) invalidate_tb();
}

/* ------------------------------------------------------------------ */
/* taskbar actions                                                     */
/* ------------------------------------------------------------------ */

static int popup_menu(HWND owner, HMENU m, BOOL bottom)
{
    POINT pt;
    int cmd;
    GetCursorPos(&pt);
    in_menu = TRUE;
    SetForegroundWindow(owner);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | (bottom ? TPM_BOTTOMALIGN : TPM_TOPALIGN),
                              pt.x, pt.y, 0, owner, NULL);
    PostMessageW(owner, WM_NULL, 0, 0);
    in_menu = FALSE;
    DestroyMenu(m);
    return cmd;
}

static void toggle_desktop(void)
{
    int i;
    sm_hide();
    if (!desk_active)
    {
        desk_cnt = 0;
        for (i = 0; i < btn_cnt; i++)
            if (!IsIconic(btns[i].hwnd)) { desk_hidden[desk_cnt++] = btns[i].hwnd; ShowWindowAsync(btns[i].hwnd, SW_MINIMIZE); }
        desk_active = desk_cnt > 0;
    }
    else
    {
        for (i = desk_cnt - 1; i >= 0; i--)
            if (IsWindow(desk_hidden[i])) ShowWindowAsync(desk_hidden[i], SW_RESTORE);
        desk_active = FALSE;
    }
}

static void btn_click(int i)
{
    HWND h;
    if (i < 0 || i >= btn_cnt) return;
    h = btns[i].hwnd;
    desk_active = FALSE;
    if (IsIconic(h))
    {
        ShowWindowAsync(h, SW_RESTORE);
        SetForegroundWindow(h);
    }
    else if (h == active_win) ShowWindowAsync(h, SW_MINIMIZE);
    else
    {
        SetForegroundWindow(h);
        BringWindowToTop(h);
    }
}

static void btn_move(int from, int to)
{
    W10BTN t = btns[from];
    if (from == to) return;
    if (from < to) MoveMemory(&btns[from], &btns[from + 1], (to - from) * sizeof(W10BTN));
    else MoveMemory(&btns[to + 1], &btns[to], (from - to) * sizeof(W10BTN));
    btns[to] = t;
}

static void btn_menu(int i)
{
    HMENU m;
    HWND h;
    int cmd;
    if (i < 0 || i >= btn_cnt) return;
    h = btns[i].hwnd;
    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Restore");
    AppendMenuW(m, MF_STRING, 2, L"Minimize");
    AppendMenuW(m, MF_STRING, 3, L"Maximize");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 4, L"Close window");
    cmd = popup_menu(tb_hwnd, m, !cfg.top);
    if (!IsWindow(h)) return;
    switch (cmd)
    {
    case 1: ShowWindowAsync(h, SW_RESTORE); SetForegroundWindow(h); break;
    case 2: ShowWindowAsync(h, SW_MINIMIZE); break;
    case 3: ShowWindowAsync(h, SW_MAXIMIZE); SetForegroundWindow(h); break;
    case 4: PostMessageW(h, WM_CLOSE, 0, 0); break;
    }
}

static void tb_menu(void)
{
    HMENU m = CreatePopupMenu();
    int cmd;
    AppendMenuW(m, MF_STRING, 1, L"Taskbar settings");
    AppendMenuW(m, MF_STRING, 2, desk_active ? L"Show open windows" : L"Show the desktop");
    AppendMenuW(m, MF_STRING, 3, L"Task Manager");
    cmd = popup_menu(tb_hwnd, m, !cfg.top);
    if (cmd == 1) open_settings();
    else if (cmd == 2) toggle_desktop();
    else if (cmd == 3) ShellExecuteW(NULL, L"open", L"taskmgr.exe", NULL, NULL, SW_SHOWNORMAL);
}

static void tb_click(int part, int idx)
{
    switch (part)
    {
    case 1: sm_toggle(); break;
    case 2: sm_hide(); btn_click(idx); break;
    case 3:
        sm_hide();
        if (idx >= 0 && idx < tray_vis_cnt && tray_vis[idx] < tray_cnt)
        {
            const W10TRAY *t = &tray[tray_vis[idx]];
            tray_notify(t, WM_LBUTTONDOWN);
            tray_notify(t, WM_LBUTTONUP);
        }
        break;
    case 5: toggle_desktop(); break;
    }
}

static LRESULT CALLBACK tb_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mdc;
        HBITMAP bm;
        HGDIOBJ ob;
        RECT rc;
        GetClientRect(hwnd, &rc);
        mdc = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        ob = SelectObject(mdc, bm);
        tb_paint(mdc, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bm);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), part, idx;
        if (!tracking)
        {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = hwnd; tme.dwHoverTime = 0;
            if (TrackMouseEvent(&tme)) tracking = TRUE;
        }
        if (down_part == 2 && (wp & MK_LBUTTON))
        {
            if (!dragging && iabs(x - drag_x0) > 8) dragging = TRUE;
            if (dragging && btn_cnt > 0 && btn_w_cur > 0)
            {
                int slot = iclamp((x - r_area.left) / btn_w_cur, 0, btn_cnt - 1);
                if (slot != down_idx) { btn_move(down_idx, slot); down_idx = slot; }
                invalidate_tb();
                return 0;
            }
        }
        tb_hit(x, y, &part, &idx);
        if (part != hot_part || idx != hot_idx) { hot_part = part; hot_idx = idx; invalidate_tb(); }
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking = FALSE;
        hot_part = 0; hot_idx = -1;
        invalidate_tb();
        return 0;

    case WM_LBUTTONDOWN:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), part, idx;
        tb_hit(x, y, &part, &idx);
        down_part = part; down_idx = idx; dragging = FALSE; drag_x0 = x;
        if (part == 2) { RECT rc; if (btn_rect(idx, &rc)) drag_off = x - rc.left; }
        if (part != 1) sm_hide();
        SetCapture(hwnd);
        invalidate_tb();
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), part, idx;
        BOOL was_drag = dragging;
        int dp = down_part, di = down_idx;
        dragging = FALSE;
        down_part = 0; down_idx = -1;
        ReleaseCapture();
        tb_hit(x, y, &part, &idx);
        if (!was_drag && dp && part == dp && (dp != 2 && dp != 3 ? TRUE : idx == di)) tb_click(part, idx);
        hot_part = part; hot_idx = idx;
        invalidate_tb();
        return 0;
    }

    case WM_CAPTURECHANGED:
        dragging = FALSE;
        down_part = 0; down_idx = -1;
        return 0;

    case WM_RBUTTONUP:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), part, idx;
        tb_hit(x, y, &part, &idx);
        if (part == 2) btn_menu(idx);
        else if (part == 3 && idx >= 0 && idx < tray_vis_cnt && tray_vis[idx] < tray_cnt)
        {
            const W10TRAY *t = &tray[tray_vis[idx]];
            tray_notify(t, WM_RBUTTONDOWN);
            tray_notify(t, WM_RBUTTONUP);
            if (t->version >= 4) tray_notify(t, WM_CONTEXTMENU);
        }
        else tb_menu();
        return 0;
    }

    case WM_MBUTTONUP:
    {
        int part, idx;
        tb_hit((short)LOWORD(lp), (short)HIWORD(lp), &part, &idx);
        if (part == 2 && idx >= 0 && idx < btn_cnt) PostMessageW(btns[idx].hwnd, WM_CLOSE, 0, 0);
        return 0;
    }

    case WM_TIMER:
        if (wp == TID_POLL) tb_poll();
        else if (wp == TID_ANIM) tb_anim();
        else if (wp == TID_STABLE)
        {
            HKEY k = open_key();
            KillTimer(hwnd, TID_STABLE);
            if (k) { reg_set(k, L"BootFails", 0); RegCloseKey(k); }
        }
        return 0;

    case WM_HOTKEY:
        sm_toggle();
        return 0;

    case WM_DISPLAYCHANGE:
        tb_apply_cfg(TRUE);
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Start panel                                                         */
/* ------------------------------------------------------------------ */

#define PB_W        48
#define PB_WX       256
#define LIST_W      288
#define TILE_SZ     100
#define TILE_GAP    4
#define TILES_W     (3 * TILE_SZ + 2 * TILE_GAP + 24)
#define SM_FULL_W   (PB_W + LIST_W + TILES_W)
#define SEARCH_H    36
#define SEARCH_PAD  8
#define LIST_TOP    (SEARCH_PAD * 2 + SEARCH_H)
#define ROW_H       40
#define HDR_H       28
#define TILES_TOP   44
#define FLY_W       170
#define FLY_H       40

enum { K_NONE = 0, K_PB, K_ROW, K_TILE, K_SEARCH, K_FLY };

static BOOL sm_visible, pb_exp, power_open, sm_tracking, sm_got_focus;
static DWORD sm_show_tick, sm_hide_tick;
static int sm_w, sm_h;
static int s_hot_kind, s_hot_idx;
static BOOL ds_active, ds_moved;
static int ds_y0, ds_scroll0, ds_region, ds_kind, ds_idx;

static BOOL sm_is_open(void) { return sm_visible; }

static void tile_rect(int i, RECT *rc)
{
    int col = i % 3, row = i / 3;
    rc->left = PB_W + LIST_W + 12 + col * (TILE_SZ + TILE_GAP);
    rc->top = TILES_TOP + row * (TILE_SZ + TILE_GAP) - tile_scroll;
    rc->right = rc->left + TILE_SZ;
    rc->bottom = rc->top + TILE_SZ;
}

static int tiles_h(void) { return TILES_TOP + ((pin_cnt + 2) / 3) * (TILE_SZ + TILE_GAP) + 8; }

static void clamp_scroll(void)
{
    list_scroll = iclamp(list_scroll, 0, imax(list_h - (sm_h - LIST_TOP), 0));
    tile_scroll = iclamp(tile_scroll, 0, imax(tiles_h() - sm_h, 0));
}

static void pb_item_rect(int idx, int H, RECT *rc)
{
    int w = pb_exp ? PB_WX : PB_W;
    rc->left = 0;
    rc->right = w;
    if (idx == 0) { rc->top = 0; rc->bottom = 48; }
    else { rc->bottom = H - (3 - idx) * 48; rc->top = rc->bottom - 48; }
}

static void fly_rect(int i, RECT *rc)
{
    rc->left = pb_exp ? PB_WX : PB_W;
    rc->right = rc->left + FLY_W;
    rc->top = sm_h - 2 * FLY_H + i * FLY_H;
    rc->bottom = rc->top + FLY_H;
}

static BOOL in_rect(const RECT *rc, int x, int y) { return x >= rc->left && x < rc->right && y >= rc->top && y < rc->bottom; }

static void sm_hit(int x, int y, int *kind, int *idx)
{
    RECT rc;
    int i, yy, pbw = pb_exp ? PB_WX : PB_W;

    *kind = K_NONE;
    *idx = -1;
    if (power_open)
        for (i = 0; i < 2; i++)
        {
            fly_rect(i, &rc);
            if (in_rect(&rc, x, y)) { *kind = K_FLY; *idx = i; return; }
        }
    if (x < pbw)
    {
        for (i = 0; i < 4; i++)
        {
            pb_item_rect(i, sm_h, &rc);
            if (in_rect(&rc, x, y)) { *kind = K_PB; *idx = i; return; }
        }
        return;
    }
    if (x < PB_W + LIST_W)
    {
        if (y < LIST_TOP)
        {
            if (y >= SEARCH_PAD && y < SEARCH_PAD + SEARCH_H) *kind = K_SEARCH;
            return;
        }
        yy = LIST_TOP - list_scroll;
        for (i = 0; i < row_cnt; i++)
        {
            int rh = rows[i].type == 0 ? HDR_H : ROW_H;
            if (y >= yy && y < yy + rh)
            {
                if (rows[i].type != 0) { *kind = K_ROW; *idx = i; }
                return;
            }
            yy += rh;
        }
        return;
    }
    if (y < 36) return;
    for (i = 0; i < pin_cnt; i++)
    {
        tile_rect(i, &rc);
        if (in_rect(&rc, x, y)) { *kind = K_TILE; *idx = i; return; }
    }
}

/* ----- glyphs for the power bar, drawn with plain GDI ----- */

static void draw_hamburger(HDC dc, int cx, int cy)
{
    fill(dc, cx - 9, cy - 7, cx + 9, cy - 5, COL_WHITE);
    fill(dc, cx - 9, cy - 1, cx + 9, cy + 1, COL_WHITE);
    fill(dc, cx - 9, cy + 5, cx + 9, cy + 7, COL_WHITE);
}

static void draw_folder(HDC dc, int cx, int cy)
{
    HPEN p = mkpen(1, COL_WHITE);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, cx - 9, cy - 6, NULL);
    LineTo(dc, cx - 9, cy + 7);
    LineTo(dc, cx + 9, cy + 7);
    LineTo(dc, cx + 9, cy - 4);
    LineTo(dc, cx + 1, cy - 4);
    LineTo(dc, cx - 1, cy - 6);
    LineTo(dc, cx - 9, cy - 6);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void draw_gear(HDC dc, int cx, int cy)
{
    static const int d[8][4] = { {7,0,9,0}, {5,5,7,7}, {0,7,0,9}, {-5,5,-7,7}, {-7,0,-9,0}, {-5,-5,-7,-7}, {0,-7,0,-9}, {5,-5,7,-7} };
    HPEN p = mkpen(2, COL_WHITE);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int i;
    Ellipse(dc, cx - 6, cy - 6, cx + 7, cy + 7);
    Ellipse(dc, cx - 2, cy - 2, cx + 3, cy + 3);
    for (i = 0; i < 8; i++)
    {
        MoveToEx(dc, cx + d[i][0], cy + d[i][1], NULL);
        LineTo(dc, cx + d[i][2], cy + d[i][3]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void draw_power(HDC dc, int cx, int cy)
{
    HPEN p = mkpen(2, COL_WHITE);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, cx - 8, cy - 8, cx + 9, cy + 9, cx - 5, cy - 7, cx + 5, cy - 7);
    MoveToEx(dc, cx, cy - 10, NULL);
    LineTo(dc, cx, cy - 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void draw_magnifier(HDC dc, int x, int y, COLORREF c)
{
    HPEN p = mkpen(2, c);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x, y, x + 11, y + 11);
    MoveToEx(dc, x + 10, y + 10, NULL);
    LineTo(dc, x + 16, y + 16);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

/* ----- painting ----- */

static void sm_paint_list(HDC dc, int H)
{
    int i, y = LIST_TOP - list_scroll, view = H - LIST_TOP;
    WCHAR buf[160];

    SaveDC(dc);
    IntersectClipRect(dc, PB_W, LIST_TOP, PB_W + LIST_W, H);
    for (i = 0; i < row_cnt; i++)
    {
        int rh = rows[i].type == 0 ? HDR_H : ROW_H;
        if (y + rh > LIST_TOP && y < H)
        {
            if (rows[i].type == 0)
            {
                WCHAR s[2];
                s[0] = rows[i].letter == '#' ? L'&' : (WCHAR)rows[i].letter; s[1] = 0;
                draw_text(dc, s, PB_W + 16, y, 40, rh, f_bold, COL_WHITE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            else
            {
                BOOL hot = (s_hot_kind == K_ROW && s_hot_idx == i);
                if (hot) fill(dc, PB_W, y, PB_W + LIST_W, y + rh, COL_ROW_HOT);
                if (rows[i].type == 1)
                {
                    W10APP *a = &apps[rows[i].app];
                    draw_app_icon(dc, PB_W + 12, y + 8, 24, a->icon, a->name);
                    draw_text(dc, a->name, PB_W + 48, y, LIST_W - 60, rh, f_ui, COL_WHITE,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                else
                {
                    draw_badge(dc, PB_W + 12, y + 8, 24, accent_col(), L'>');
                    wsprintfW(buf, L"Run \"%s\"", query);
                    draw_text(dc, buf, PB_W + 48, y, LIST_W - 60, rh, f_ui, COL_WHITE,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
        }
        y += rh;
    }
    RestoreDC(dc, -1);
    if (list_h > view)
    {
        int th = imax(view * view / list_h, 24);
        int ty = LIST_TOP + (list_scroll * (view - th)) / imax(list_h - view, 1);
        fill(dc, PB_W + LIST_W - 5, ty, PB_W + LIST_W - 2, ty + th, RGB(112, 112, 112));
    }
}

static void sm_paint_tiles(HDC dc, int H)
{
    int i;
    int tx = PB_W + LIST_W;

    SaveDC(dc);
    IntersectClipRect(dc, tx, 0, tx + TILES_W, H);
    draw_text(dc, L"Pinned", tx + 12, 12 - tile_scroll, 200, 24, f_bold, COL_WHITE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (i = 0; i < pin_cnt; i++)
    {
        RECT rc;
        BOOL hot = (s_hot_kind == K_TILE && s_hot_idx == i);
        tile_rect(i, &rc);
        if (rc.bottom < 36 || rc.top > H) continue;
        fill(dc, rc.left, rc.top, rc.right, rc.bottom, hot ? lighten(accent_col(), 22) : accent_col());
        draw_app_icon(dc, rc.left + (TILE_SZ - 36) / 2, rc.top + 16, 36, pins[i].icon, pins[i].name);
        draw_text(dc, pins[i].name, rc.left + 8, rc.bottom - 28, TILE_SZ - 14, 22, f_small, COL_WHITE,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (hot)
        {
            fill(dc, rc.left, rc.top, rc.right, rc.top + 2, COL_WHITE);
            fill(dc, rc.left, rc.bottom - 2, rc.right, rc.bottom, COL_WHITE);
            fill(dc, rc.left, rc.top, rc.left + 2, rc.bottom, COL_WHITE);
            fill(dc, rc.right - 2, rc.top, rc.right, rc.bottom, COL_WHITE);
        }
    }
    RestoreDC(dc, -1);
}

static void sm_paint_search(HDC dc)
{
    int x0 = PB_W + SEARCH_PAD, y0 = SEARCH_PAD, w = LIST_W - 2 * SEARCH_PAD;
    COLORREF ac = accent_col();

    fill(dc, x0, y0, x0 + w, y0 + SEARCH_H, COL_SEARCH_BG);
    fill(dc, x0, y0, x0 + w, y0 + 2, ac);
    fill(dc, x0, y0 + SEARCH_H - 2, x0 + w, y0 + SEARCH_H, ac);
    fill(dc, x0, y0, x0 + 2, y0 + SEARCH_H, ac);
    fill(dc, x0 + w - 2, y0, x0 + w, y0 + SEARCH_H, ac);
    if (qlen)
    {
        SIZE sz;
        HGDIOBJ of = SelectObject(dc, f_ui);
        draw_text(dc, query, x0 + 10, y0, w - 48, SEARCH_H, f_ui, COL_SEARCH_TXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        GetTextExtentPoint32W(dc, query, qlen, &sz);
        SelectObject(dc, of);
        if (sz.cx < w - 50) fill(dc, x0 + 11 + sz.cx, y0 + 8, x0 + 12 + sz.cx, y0 + SEARCH_H - 8, COL_SEARCH_TXT);
    }
    else
    {
        draw_text(dc, L"Type here to search", x0 + 10, y0, w - 48, SEARCH_H, f_ui, RGB(118, 118, 118), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        fill(dc, x0 + 10, y0 + 8, x0 + 11, y0 + SEARCH_H - 8, COL_SEARCH_TXT);
    }
    draw_magnifier(dc, x0 + w - 30, y0 + 9, COL_SEARCH_TXT);
}

static void sm_paint_pb(HDC dc, int H)
{
    static const WCHAR *const labels[4] = { L"START", L"File Explorer", L"Settings", L"Power" };
    int w = pb_exp ? PB_WX : PB_W, i;
    RECT rc;

    fill(dc, 0, 0, w, H, pb_exp ? COL_PB_EXP : COL_PB_BG);
    for (i = 0; i < 4; i++)
    {
        int cx = 24, cy;
        pb_item_rect(i, H, &rc);
        cy = (rc.top + rc.bottom) / 2;
        if ((s_hot_kind == K_PB && s_hot_idx == i) || (i == 3 && power_open))
            fill(dc, rc.left, rc.top, rc.right, rc.bottom, COL_ROW_HOT);
        switch (i)
        {
        case 0: draw_hamburger(dc, cx, cy); break;
        case 1: draw_folder(dc, cx, cy); break;
        case 2: draw_gear(dc, cx, cy); break;
        default: draw_power(dc, cx, cy); break;
        }
        if (pb_exp)
            draw_text(dc, labels[i], 56, rc.top, w - 60, rc.bottom - rc.top, i == 0 ? f_small : f_ui, COL_WHITE,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void sm_paint_fly(HDC dc)
{
    static const WCHAR *const labels[2] = { L"Shut down", L"Restart" };
    int i;
    RECT rc;
    fly_rect(0, &rc);
    fill(dc, rc.left - 1, rc.top - 1, rc.left + FLY_W + 1, rc.top + 2 * FLY_H + 1, RGB(72, 72, 72));
    for (i = 0; i < 2; i++)
    {
        fly_rect(i, &rc);
        fill(dc, rc.left, rc.top, rc.right, rc.bottom, (s_hot_kind == K_FLY && s_hot_idx == i) ? COL_ROW_HOT : COL_FLY_BG);
        draw_text(dc, labels[i], rc.left + 16, rc.top, FLY_W - 20, FLY_H, f_ui, COL_WHITE, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void sm_paint(HDC dc, int W, int H)
{
    fill(dc, 0, 0, W, H, COL_START_BG);
    sm_paint_list(dc, H);
    sm_paint_tiles(dc, H);
    fill(dc, 0, 0, PB_W + LIST_W, LIST_TOP, COL_START_BG);
    sm_paint_search(dc);
    sm_paint_pb(dc, H);
    if (power_open) sm_paint_fly(dc);
}

/* ----- actions ----- */

static void sm_invalidate(void) { if (sm_hwnd) InvalidateRect(sm_hwnd, NULL, FALSE); }

static void run_wineboot(int restart)
{
    ShellExecuteW(NULL, L"open", L"wineboot.exe", restart ? L"-e -f" : L"-e -f -s", NULL, SW_HIDE);
}

static void run_query(void)
{
    WCHAR cmd[130];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!qlen) return;
    lstrcpynW(cmd, query, 128);
    sm_hide();
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else ShellExecuteW(NULL, L"open", query, NULL, NULL, SW_SHOWNORMAL);
}

static void query_changed(void)
{
    list_scroll = 0;
    rebuild_rows();
    SetTimer(sm_hwnd, TID_ICONS, 15, NULL);
    sm_invalidate();
}

static void sm_activate(int kind, int idx)
{
    switch (kind)
    {
    case K_PB:
        if (idx == 0) { pb_exp = !pb_exp; power_open = FALSE; }
        else if (idx == 1) { sm_hide(); ShellExecuteW(NULL, L"open", L"explorer.exe", L"C:\\", NULL, SW_SHOWNORMAL); }
        else if (idx == 2) { sm_hide(); open_settings(); }
        else power_open = !power_open;
        break;
    case K_FLY:
        sm_hide();
        run_wineboot(idx);
        break;
    case K_ROW:
        if (idx < 0 || idx >= row_cnt) break;
        if (rows[idx].type == 1)
        {
            WCHAR path[MAX_PATH];
            lstrcpyW(path, apps[rows[idx].app].path);
            sm_hide();
            ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        }
        else if (rows[idx].type == 2) run_query();
        break;
    case K_TILE:
        if (idx >= 0 && idx < pin_cnt)
        {
            WCHAR path[MAX_PATH];
            lstrcpyW(path, pins[idx].path);
            sm_hide();
            ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        }
        break;
    }
    sm_invalidate();
}

static void sm_context(int kind, int idx)
{
    HMENU m;
    int cmd;
    WCHAR path[MAX_PATH], name[96];

    if (kind == K_ROW && idx >= 0 && idx < row_cnt && rows[idx].type == 1)
    {
        lstrcpyW(path, apps[rows[idx].app].path);
        lstrcpyW(name, apps[rows[idx].app].name);
        m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Open");
        AppendMenuW(m, MF_STRING, 2, pin_find(path) >= 0 ? L"Unpin from Start" : L"Pin to Start");
        cmd = popup_menu(sm_hwnd, m, FALSE);
        if (cmd == 1) sm_activate(K_ROW, idx);
        else if (cmd == 2)
        {
            int p = pin_find(path);
            if (p >= 0) pin_remove(p);
            else pin_add(path, name);
            pins_save();
            SetTimer(sm_hwnd, TID_ICONS, 15, NULL);
            clamp_scroll();
            sm_invalidate();
        }
    }
    else if (kind == K_TILE && idx >= 0 && idx < pin_cnt)
    {
        m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Open");
        AppendMenuW(m, MF_STRING, 2, L"Unpin from Start");
        cmd = popup_menu(sm_hwnd, m, FALSE);
        if (cmd == 1) sm_activate(K_TILE, idx);
        else if (cmd == 2)
        {
            pin_remove(idx);
            pins_save();
            clamp_scroll();
            sm_invalidate();
        }
    }
}

static void paste_query(void)
{
    HANDLE h;
    const WCHAR *p;
    if (!OpenClipboard(sm_hwnd)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h && (p = (const WCHAR *)GlobalLock(h)))
    {
        while (*p && qlen < 100)
        {
            if (*p >= 32) query[qlen++] = *p;
            p++;
        }
        query[qlen] = 0;
        GlobalUnlock(h);
    }
    CloseClipboard();
}

static void sm_enter(void)
{
    int i;
    if (!qlen) return;
    for (i = 0; i < row_cnt; i++)
        if (rows[i].type == 1) { sm_activate(K_ROW, i); return; }
    run_query();
}

static void sm_show(void)
{
    int x = 0, y;

    if (sm_visible || fs_hidden || !sm_hwnd) return;
    if (!last_scan || GetTickCount() - last_scan > 30000) apps_rescan();
    qlen = 0;
    query[0] = 0;
    list_scroll = tile_scroll = 0;
    pb_exp = FALSE;
    power_open = FALSE;
    s_hot_kind = K_NONE;
    s_hot_idx = -1;
    rebuild_rows();
    sm_w = imin(SM_FULL_W, scr_w);
    sm_h = iclamp(scr_h - cfg.height - 6, 280, 640);
    y = cfg.top ? cfg.height : scr_h - cfg.height - sm_h;
    clamp_scroll();
    if (ah_hidden) tb_slide(FALSE);
    sm_visible = TRUE;
    sm_show_tick = GetTickCount();
    SetWindowPos(sm_hwnd, HWND_TOPMOST, x, y, sm_w, sm_h, SWP_SHOWWINDOW);
    SetForegroundWindow(sm_hwnd);
    SetFocus(sm_hwnd);
    sm_got_focus = (GetForegroundWindow() == sm_hwnd);
    SetTimer(sm_hwnd, TID_ICONS, 15, NULL);
    sm_invalidate();
    invalidate_tb();
}

static void sm_hide(void)
{
    if (!sm_visible) return;
    sm_visible = FALSE;
    power_open = FALSE;
    sm_hide_tick = GetTickCount();
    KillTimer(sm_hwnd, TID_ICONS);
    ShowWindow(sm_hwnd, SW_HIDE);
    invalidate_tb();
}

static void sm_toggle(void)
{
    if (sm_visible) sm_hide();
    else if (GetTickCount() - sm_hide_tick > 250) sm_show();
}

/* called from the taskbar poll: close the panel when focus went elsewhere */
static void sm_poll(void)
{
    if (!sm_visible || in_menu) return;
    if (GetForegroundWindow() == sm_hwnd) { sm_got_focus = TRUE; return; }
    if (sm_got_focus && GetTickCount() - sm_show_tick > 600) sm_hide();
}

static LRESULT CALLBACK sm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mdc;
        HBITMAP bm;
        HGDIOBJ ob;
        RECT rc;
        GetClientRect(hwnd, &rc);
        mdc = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        ob = SelectObject(mdc, bm);
        sm_paint(mdc, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bm);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
        {
            if (sm_visible && !in_menu && GetTickCount() - sm_show_tick > 500) sm_hide();
        }
        else sm_got_focus = TRUE;
        return 0;

    case WM_MOUSEMOVE:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), k, i;
        if (!sm_tracking)
        {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = hwnd; tme.dwHoverTime = 0;
            if (TrackMouseEvent(&tme)) sm_tracking = TRUE;
        }
        if (ds_active && (wp & MK_LBUTTON))
        {
            if (!ds_moved && ds_region && iabs(y - ds_y0) > 8) ds_moved = TRUE;
            if (ds_moved)
            {
                if (ds_region == 1) list_scroll = ds_scroll0 + (ds_y0 - y);
                else tile_scroll = ds_scroll0 + (ds_y0 - y);
                clamp_scroll();
                sm_invalidate();
                return 0;
            }
        }
        sm_hit(x, y, &k, &i);
        if (k != s_hot_kind || i != s_hot_idx) { s_hot_kind = k; s_hot_idx = i; sm_invalidate(); }
        return 0;
    }

    case WM_MOUSELEAVE:
        sm_tracking = FALSE;
        s_hot_kind = K_NONE;
        s_hot_idx = -1;
        sm_invalidate();
        return 0;

    case WM_LBUTTONDOWN:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), k, i;
        sm_hit(x, y, &k, &i);
        ds_active = TRUE; ds_moved = FALSE; ds_y0 = y; ds_kind = k; ds_idx = i;
        ds_region = 0;
        if (x >= PB_W && x < PB_W + LIST_W && y >= LIST_TOP && k != K_FLY) ds_region = 1;
        else if (x >= PB_W + LIST_W) ds_region = 2;
        ds_scroll0 = (ds_region == 1) ? list_scroll : tile_scroll;
        SetCapture(hwnd);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), k, i;
        BOOL act = ds_active && !ds_moved;
        ds_active = FALSE;
        ReleaseCapture();
        if (!act) return 0;
        sm_hit(x, y, &k, &i);
        if (k == ds_kind && i == ds_idx && k != K_NONE && k != K_SEARCH) sm_activate(k, i);
        else if (power_open && k != K_FLY && !(k == K_PB && i == 3)) { power_open = FALSE; sm_invalidate(); }
        return 0;
    }

    case WM_CAPTURECHANGED:
        ds_active = FALSE;
        return 0;

    case WM_RBUTTONUP:
    {
        int k, i;
        sm_hit((short)LOWORD(lp), (short)HIWORD(lp), &k, &i);
        sm_context(k, i);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        POINT pt;
        int d = (short)HIWORD(wp);
        pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        if (pt.x >= PB_W + LIST_W) tile_scroll -= d * 80 / 120;
        else list_scroll -= d * 80 / 120;
        clamp_scroll();
        sm_invalidate();
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            if (power_open) { power_open = FALSE; sm_invalidate(); }
            else if (qlen) { qlen = 0; query[0] = 0; query_changed(); }
            else sm_hide();
        }
        return 0;

    case WM_CHAR:
        if (wp == 8) { if (qlen) { query[--qlen] = 0; query_changed(); } }
        else if (wp == 13) sm_enter();
        else if (wp == 22) { paste_query(); query_changed(); }
        else if (wp >= 32 && wp != 127 && qlen < 100)
        {
            query[qlen++] = (WCHAR)wp;
            query[qlen] = 0;
            query_changed();
        }
        return 0;

    case WM_TIMER:
        if (wp == TID_ICONS)
        {
            if (!load_some_icons(2)) KillTimer(hwnd, TID_ICONS);
            sm_invalidate();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* taskbar settings window                                             */
/* ------------------------------------------------------------------ */

enum
{
    ID_HEIGHT = 101, ID_AUTOHIDE, ID_REVEAL, ID_RDELAY, ID_HDELAY, ID_BTNW, ID_POS, ID_ACCENT,
    ID_RESET, ID_CLOSE, ID_VAL_BASE = 300
};

typedef struct { int id, row; const WCHAR *label, *unit; int lo, hi, step; int *val; } W10SLIDER;

static const W10SLIDER sliders[] =
{
    { ID_HEIGHT, 0, L"Taskbar height",        L"px", 28, 96,   2,  &cfg.height },
    { ID_REVEAL, 2, L"Reveal bar thickness",  L"px", 1,  48,   1,  &cfg.reveal },
    { ID_RDELAY, 3, L"Reveal delay",          L"ms", 0,  2000, 50, &cfg.reveal_delay },
    { ID_HDELAY, 4, L"Hide delay",            L"ms", 0,  5000, 100, &cfg.hide_delay },
    { ID_BTNW,   5, L"App button width",      L"px", 90, 320,  5,  &cfg.btn_width },
};

#define ST_ROW_Y(r)     (52 + (r) * 38)

static void st_set_val(HWND hwnd, const W10SLIDER *s)
{
    WCHAR t[40];
    wsprintfW(t, L"%d %s", *s->val, s->unit);
    SetDlgItemTextW(hwnd, ID_VAL_BASE + s->id, t);
}

static void st_apply(BOOL final)
{
    cfg_dirty = TRUE;
    tb_apply_cfg(final);
    if (sm_visible) sm_hide();
}

static void st_sync_controls(HWND hwnd)
{
    int i;
    for (i = 0; i < NELEM(sliders); i++)
    {
        SendDlgItemMessageW(hwnd, sliders[i].id, TBM_SETPOS, TRUE, *sliders[i].val);
        st_set_val(hwnd, &sliders[i]);
    }
    SendDlgItemMessageW(hwnd, ID_AUTOHIDE, BM_SETCHECK, cfg.autohide ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessageW(hwnd, ID_POS, CB_SETCURSEL, cfg.top ? 1 : 0, 0);
    SendDlgItemMessageW(hwnd, ID_ACCENT, CB_SETCURSEL, cfg.accent, 0);
}

static HWND st_child(HWND p, const WCHAR *cls, const WCHAR *txt, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(ULONG_PTR)id, hinst, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)f_ui, TRUE);
    return c;
}

static LRESULT CALLBACK st_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        int i, y;
        HWND h = CreateWindowExW(0, L"STATIC", L"Taskbar", WS_CHILD | WS_VISIBLE, 16, 10, 400, 32, hwnd, NULL, hinst, NULL);
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)f_title, TRUE);
        for (i = 0; i < NELEM(sliders); i++)
        {
            y = ST_ROW_Y(sliders[i].row);
            st_child(hwnd, L"STATIC", sliders[i].label, 0, 16, y + 5, 168, 22, 0);
            h = st_child(hwnd, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 186, y, 160, 30, sliders[i].id);
            if (h)
            {
                SendMessageW(h, TBM_SETRANGE, TRUE, MAKELONG(sliders[i].lo, sliders[i].hi));
                SendMessageW(h, TBM_SETLINESIZE, 0, sliders[i].step);
                SendMessageW(h, TBM_SETPAGESIZE, 0, sliders[i].step * 5);
            }
            st_child(hwnd, L"STATIC", L"", SS_RIGHT, 350, y + 5, 78, 22, ID_VAL_BASE + sliders[i].id);
        }
        st_child(hwnd, L"BUTTON", L"Automatically hide the taskbar", BS_AUTOCHECKBOX | WS_TABSTOP,
                 16, ST_ROW_Y(1) + 3, 360, 24, ID_AUTOHIDE);
        y = ST_ROW_Y(6);
        st_child(hwnd, L"STATIC", L"Taskbar location", 0, 16, y + 5, 168, 22, 0);
        h = st_child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 186, y, 240, 120, ID_POS);
        if (h) { SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Bottom"); SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Top"); }
        y = ST_ROW_Y(7);
        st_child(hwnd, L"STATIC", L"Accent colour", 0, 16, y + 5, 168, 22, 0);
        h = st_child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 186, y, 240, 220, ID_ACCENT);
        if (h) for (i = 0; i < NELEM(accent_names); i++) SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)accent_names[i]);
        y = ST_ROW_Y(8) + 8;
        st_child(hwnd, L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, 16, y, 150, 32, ID_RESET);
        st_child(hwnd, L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, 296, y, 130, 32, ID_CLOSE);
        st_sync_controls(hwnd);
        return 0;
    }

    case WM_HSCROLL:
    {
        int id = GetDlgCtrlID((HWND)lp), i;
        for (i = 0; i < NELEM(sliders); i++)
            if (sliders[i].id == id)
            {
                *sliders[i].val = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
                st_set_val(hwnd, &sliders[i]);
                st_apply(LOWORD(wp) == TB_ENDTRACK);
                break;
            }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_AUTOHIDE && code == BN_CLICKED)
        {
            cfg.autohide = (SendDlgItemMessageW(hwnd, ID_AUTOHIDE, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            st_apply(TRUE);
        }
        else if (id == ID_POS && code == CBN_SELCHANGE)
        {
            cfg.top = (SendDlgItemMessageW(hwnd, ID_POS, CB_GETCURSEL, 0, 0) == 1) ? 1 : 0;
            st_apply(TRUE);
        }
        else if (id == ID_ACCENT && code == CBN_SELCHANGE)
        {
            int s = (int)SendDlgItemMessageW(hwnd, ID_ACCENT, CB_GETCURSEL, 0, 0);
            if (s >= 0) cfg.accent = s;
            cfg_dirty = TRUE;
            invalidate_tb();
            sm_invalidate();
        }
        else if (id == ID_RESET && code == BN_CLICKED)
        {
            cfg_defaults();
            st_sync_controls(hwnd);
            st_apply(TRUE);
            invalidate_tb();
        }
        else if (id == ID_CLOSE && code == BN_CLICKED) DestroyWindow(hwnd);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        st_hwnd = NULL;
        cfg_save();
        tb_apply_cfg(TRUE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void open_settings(void)
{
    RECT rc;
    int w, h;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    if (st_hwnd)
    {
        ShowWindow(st_hwnd, SW_RESTORE);
        SetForegroundWindow(st_hwnd);
        return;
    }
    SetRect(&rc, 0, 0, 444, ST_ROW_Y(8) + 56);
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    st_hwnd = CreateWindowExW(0, SETT_CLASS, L"Taskbar settings", style,
                              imax((scr_w - w) / 2, 0), imax((scr_h - cfg.height - h) / 2, 0), w, h,
                              NULL, NULL, hinst, NULL);
    if (st_hwnd)
    {
        ShowWindow(st_hwnd, SW_SHOW);
        SetForegroundWindow(st_hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* entry points called from desktop.c                                  */
/* ------------------------------------------------------------------ */

/* Returns TRUE when the Windows 10 style shell took over; FALSE keeps Wine's own taskbar. */
BOOL w10_shell_start(BOOL enable_shell)
{
    HKEY k;
    DWORD fails;
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;

    if (!enable_shell || shell_on) return FALSE;
    if (file_exists(L"C:\\w10shell_disable")) { wlog("disabled by C:\\w10shell_disable"); return FALSE; }

    /* crash guard: two early deaths in a row -> fall back to Wine's own taskbar */
    k = open_key();
    if (k)
    {
        if (reg_get(k, L"Disabled", 0)) { wlog("disabled in registry"); RegCloseKey(k); return FALSE; }
        fails = reg_get(k, L"BootFails", 0);
        if (fails >= 2)
        {
            reg_set(k, L"Disabled", 1);
            reg_set(k, L"BootFails", 0);
            RegCloseKey(k);
            wlog("two failed starts, switching to safe mode");
            return FALSE;
        }
        reg_set(k, L"BootFails", fails + 1);
        RegCloseKey(k);
    }

    hinst = GetModuleHandleW(NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    scr_w = GetSystemMetrics(SM_CXSCREEN);
    scr_h = GetSystemMetrics(SM_CYSCREEN);
    make_fonts();
    cfg_load();
    pins_load();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpfnWndProc = tb_proc;
    wc.lpszClassName = TBAR_CLASS;
    if (!RegisterClassExW(&wc)) { wlog("RegisterClassEx taskbar failed"); return FALSE; }
    wc.lpfnWndProc = sm_proc;
    wc.lpszClassName = START_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = st_proc;
    wc.lpszClassName = SETT_CLASS;
    wc.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    tb_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TBAR_CLASS, L"W10 Taskbar",
                              WS_POPUP, 0, scr_h - cfg.height, scr_w, cfg.height, NULL, NULL, hinst, NULL);
    if (!tb_hwnd) { wlog("CreateWindow taskbar failed"); return FALSE; }
    sm_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, START_CLASS, L"Start", WS_POPUP,
                              0, 0, 10, 10, NULL, NULL, hinst, NULL);

    update_clock();
    tb_apply_cfg(TRUE);
    SetTimer(tb_hwnd, TID_POLL, POLL_MS, NULL);
    SetTimer(tb_hwnd, TID_STABLE, 5000, NULL);
    RegisterHotKey(tb_hwnd, 1, MOD_CONTROL, VK_ESCAPE);
    shell_on = TRUE;
    wlog("W10 shell started %dx%d", scr_w, scr_h);
    return TRUE;
}

/* Call after Wine's initialize_systray(): listen on its Shell_TrayWnd and keep it hidden. */
void w10_shell_attach_tray(void)
{
    HWND h;
    DWORD pid = 0;

    if (!shell_on) return;
    h = FindWindowW(L"Shell_TrayWnd", NULL);
    if (!h || h == tb_hwnd) { wlog("no Shell_TrayWnd to attach to"); return; }
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) { wlog("Shell_TrayWnd belongs to another process"); return; }
    tray_old_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)tray_sub_proc);
    if (!tray_old_proc) { wlog("could not subclass Shell_TrayWnd"); return; }
    tray_hwnd = h;
    ShowWindow(h, SW_HIDE);
    wlog("attached to Shell_TrayWnd %p", (void *)h);
}
