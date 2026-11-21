/*
 * WinShell - Windows 7 style Taskbar + Start Panel + Desktop + Titlebar skin
 * for Winlator (Wine). Pure Win32 C. Build: see README.md / GitHub Actions.
 *
 * Config file: winshell.ini (next to the .exe)
 */
#define UNICODE
#define _UNICODE
#define COBJMACROS
#undef WINVER
#define WINVER 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#define _WIN32_IE 0x0700
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <wchar.h>

/* ---- minimal GDI+ flat API (PNG/JPG/GIF/BMP loading) ---- */
typedef struct { UINT32 ver; void *cb; BOOL noThread; BOOL noCodecs; } GpInput;
extern int WINAPI GdiplusStartup(ULONG_PTR *tok, const GpInput *in, void *out);
extern int WINAPI GdipCreateBitmapFromFile(const WCHAR *f, void **bmp);
extern int WINAPI GdipCreateHBITMAPFromBitmap(void *bmp, HBITMAP *h, DWORD bg);
extern int WINAPI GdipDisposeImage(void *img);

#define MAXPIN  32
#define MAXTASK 96
#define MAXDECO 64
#define CAPH    30           /* custom titlebar height */
#define PW 420               /* start panel size */
#define PH 490
#define LW 260               /* left pane width */
#define IH 36                /* start list item height */
#define LISTY 6
#define LISTH 400
#define H_NONE (-1)
#define H_ORB  1000
#define H_TRAY 1001
#define H_CLOCK 1002
#define H_DESK 1003
#define IDC_SEARCH 500

/* ================= globals ================= */
typedef struct {
    int tbH, tbW, tbTop, titles, autoHide, revLen, revThk, revPos;
    int showClock, showDate, tzFixed, tzOff, fullDesk, skin;
    wchar_t timeFmt[64], dateFmt[64], user[64], pic[MAX_PATH], wall[MAX_PATH];
} Cfg;
typedef struct { wchar_t path[MAX_PATH], target[MAX_PATH]; HICON ic; } Pin;
typedef struct { HWND hwnd; int pin; wchar_t title[96]; HICON icon; } Task;

static Cfg g_c;
static Pin g_p[MAXPIN]; static int g_np;
static HINSTANCE g_hi;
static wchar_t g_ini[MAX_PATH];
static HWND g_tb, g_rev, g_start, g_desk, g_set, g_search;
static RECT g_tbRect, g_origWA;
static int g_scrW, g_scrH, g_inMenu, g_showDesk;
static DWORD g_lastIn, g_startHide;
static HFONT g_f, g_fb;
static ULONG_PTR g_gdip;
static HBITMAP g_pic, g_wall;
static wchar_t g_timeStr[64], g_dateStr[64];

static void ApplyAll(void);
static void OpenSettings(int tab);
static void HideStart(void);
static void ShowStart(void);
static void SavePins(void);
static void RefreshTasks(int force);
static void Layout(void);
static void ShowDesk(void);
static void UnskinAll(void);
static void SyncDecos(void);

/* ================= small helpers ================= */
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static COLORREF Mix(COLORREF a, COLORREF b, int t, int n) {
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / n,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / n,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / n);
}
static void FillR(HDC dc, const RECT *r, COLORREF c) {
    SetDCBrushColor(dc, c); FillRect(dc, r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void VGrad(HDC dc, const RECT *r, COLORREF a, COLORREF b) {
    int h = r->bottom - r->top, i;
    for (i = 0; i < h; i++) {
        RECT l = { r->left, r->top + i, r->right, r->top + i + 1 };
        FillR(dc, &l, Mix(a, b, i, h > 1 ? h - 1 : 1));
    }
}
static void RRect(HDC dc, RECT r, COLORREF fill, COLORREF border, int rad) {
    HBRUSH b = fill == CLR_NONE ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    if (fill != CLR_NONE) DeleteObject(b);
    DeleteObject(p);
}
static void Txt(HDC dc, const wchar_t *s, RECT r, UINT fl, COLORREF c, HFONT f) {
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fl | DT_NOPREFIX);
}
static HBITMAP LoadImg(const wchar_t *p) {
    void *b = NULL; HBITMAP h = NULL;
    if (!p || !p[0]) return NULL;
    if (GdipCreateBitmapFromFile(p, &b) == 0 && b) {
        GdipCreateHBITMAPFromBitmap(b, &h, 0xFF000000); GdipDisposeImage(b);
    }
    return h;
}
static void Launch(const wchar_t *p) { ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL); }
static void Run(const wchar_t *exe, const wchar_t *args) { ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL); }
static int PickFile(HWND o, wchar_t *out, const wchar_t *filter, const wchar_t *title) {
    OPENFILENAMEW f; ZeroMemory(&f, sizeof f); out[0] = 0;
    f.lStructSize = sizeof f; f.hwndOwner = o; f.lpstrFile = out; f.nMaxFile = MAX_PATH;
    f.lpstrFilter = filter; f.lpstrTitle = title;
    f.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&f);
}
static int TrackPop(HWND owner, HMENU m, int x, int y, UINT fl) {
    int c; g_inMenu = 1; SetForegroundWindow(owner);
    c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | fl, x, y, 0, owner, NULL);
    PostMessageW(owner, WM_NULL, 0, 0); g_inMenu = 0; DestroyMenu(m); return c;
}
#define IMG_FILTER L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0"
#define EXE_FILTER L"Programs\0*.exe;*.lnk;*.bat\0All files\0*.*\0"

/* ================= config ================= */
static int GI(const wchar_t *k, int d) { return GetPrivateProfileIntW(L"Shell", k, d, g_ini); }
static void GS(const wchar_t *k, const wchar_t *d, wchar_t *o, int n) { GetPrivateProfileStringW(L"Shell", k, d, o, n, g_ini); }
static void PS(const wchar_t *k, const wchar_t *v) { WritePrivateProfileStringW(L"Shell", k, v, g_ini); }
static void PI(const wchar_t *k, int v) { wchar_t b[24]; wsprintfW(b, L"%d", v); PS(k, b); }

static void ResolveLnk(const wchar_t *p, wchar_t *out) {
    IShellLinkW *sl = NULL; IPersistFile *pf = NULL;
    lstrcpynW(out, p, MAX_PATH);
    if (_wcsicmp(PathFindExtensionW(p), L".lnk")) return;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            if (SUCCEEDED(IPersistFile_Load(pf, p, STGM_READ))) {
                wchar_t t[MAX_PATH];
                if (SUCCEEDED(IShellLinkW_GetPath(sl, t, MAX_PATH, NULL, 0)) && t[0]) lstrcpynW(out, t, MAX_PATH);
            }
            IPersistFile_Release(pf);
        }
        IShellLinkW_Release(sl);
    }
}
static void AddPin(const wchar_t *p) {
    int i; SHFILEINFOW fi; Pin *x;
    if (g_np >= MAXPIN || !p[0]) return;
    for (i = 0; i < g_np; i++) if (!_wcsicmp(g_p[i].path, p)) return;
    x = &g_p[g_np++]; ZeroMemory(x, sizeof *x);
    lstrcpynW(x->path, p, MAX_PATH); ResolveLnk(p, x->target);
    ZeroMemory(&fi, sizeof fi);
    if (SHGetFileInfoW(p, 0, &fi, sizeof fi, SHGFI_ICON | SHGFI_LARGEICON)) x->ic = fi.hIcon;
}
static void RemovePin(int i) {
    if (i < 0 || i >= g_np) return;
    if (g_p[i].ic) DestroyIcon(g_p[i].ic);
    memmove(&g_p[i], &g_p[i + 1], (g_np - i - 1) * sizeof(Pin)); g_np--;
    SavePins(); RefreshTasks(1);
}
static void SavePins(void) {
    int i; wchar_t k[16];
    PI(L"PinCount", g_np);
    for (i = 0; i < MAXPIN; i++) { wsprintfW(k, L"Pin%d", i); PS(k, i < g_np ? g_p[i].path : NULL); }
}
static void LoadCfg(void) {
    Cfg *c = &g_c; wchar_t un[64] = L"User"; DWORD n = 64; int i, cnt; wchar_t k[16], b[MAX_PATH];
    GetUserNameW(un, &n);
    c->tbH = Clamp(GI(L"TaskbarHeight", 40), 24, 100);
    c->tbW = Clamp(GI(L"TaskbarWidthPct", 100), 20, 100);
    c->tbTop = GI(L"TaskbarTop", 0);
    c->titles = GI(L"ShowTitles", 1);
    c->autoHide = GI(L"AutoHide", 0);
    c->revLen = Clamp(GI(L"RevealLengthPct", 30), 5, 100);
    c->revThk = Clamp(GI(L"RevealThickness", 6), 2, 40);
    c->revPos = Clamp(GI(L"RevealPosPct", 50), 0, 100);
    c->showClock = GI(L"ShowClock", 1);
    c->showDate = GI(L"ShowDate", 1);
    c->tzFixed = GI(L"FixedOffset", 1);        /* India default: UTC+5:30 */
    c->tzOff = GI(L"OffsetMinutes", 330);
    c->fullDesk = GI(L"FullDesktop", 0);
    c->skin = GI(L"Win7Titlebar", 0);
    GS(L"TimeFormat", L"hh:mm tt", c->timeFmt, 64);
    GS(L"DateFormat", L"dd-MM-yyyy", c->dateFmt, 64);
    GS(L"UserName", un, c->user, 64);
    GS(L"UserPicture", L"", c->pic, MAX_PATH);
    GS(L"Wallpaper", L"", c->wall, MAX_PATH);
    cnt = GI(L"PinCount", -1);
    if (cnt < 0) {   /* first run: a few defaults */
        AddPin(L"C:\\windows\\system32\\notepad.exe");
        AddPin(L"C:\\windows\\system32\\winefile.exe");
        AddPin(L"C:\\windows\\regedit.exe");
        SavePins();
    } else for (i = 0; i < cnt && i < MAXPIN; i++) {
        wsprintfW(k, L"Pin%d", i); GS(k, L"", b, MAX_PATH); if (b[0]) AddPin(b);
    }
}
static void SaveCfg(void) {
    Cfg *c = &g_c;
    PI(L"TaskbarHeight", c->tbH); PI(L"TaskbarWidthPct", c->tbW); PI(L"TaskbarTop", c->tbTop);
    PI(L"ShowTitles", c->titles); PI(L"AutoHide", c->autoHide);
    PI(L"RevealLengthPct", c->revLen); PI(L"RevealThickness", c->revThk); PI(L"RevealPosPct", c->revPos);
    PI(L"ShowClock", c->showClock); PI(L"ShowDate", c->showDate);
    PI(L"FixedOffset", c->tzFixed); PI(L"OffsetMinutes", c->tzOff);
    PI(L"FullDesktop", c->fullDesk); PI(L"Win7Titlebar", c->skin);
    PS(L"TimeFormat", c->timeFmt); PS(L"DateFormat", c->dateFmt);
    PS(L"UserName", c->user); PS(L"UserPicture", c->pic); PS(L"Wallpaper", c->wall);
}
static void MkFonts(void) {
    g_f = CreateFontW(-13, 0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fb = CreateFontW(-13, 0, 0, 0, 700, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}
static void GetWA(RECT *r) {
    SetRect(r, 0, 0, g_scrW, g_scrH);
    if (!g_c.autoHide) { if (g_c.tbTop) r->top += g_c.tbH; else r->bottom -= g_c.tbH; }
}

/* ================= windows / tasks ================= */
static int OwnWin(HWND h) { DWORD p; GetWindowThreadProcessId(h, &p); return p == GetCurrentProcessId(); }
static int BadClass(HWND h) {
    wchar_t c[64]; GetClassNameW(h, c, 64);
    return !wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW") || !wcscmp(c, L"Shell_TrayWnd") ||
           !wcscmp(c, L"#32769") || !wcscmp(c, L"ApplicationFrameWindow");
}
static void ExeOf(HWND h, wchar_t *o) {
    DWORD pid, n = MAX_PATH; HANDLE p; o[0] = 0;
    GetWindowThreadProcessId(h, &pid);
    p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) { QueryFullProcessImageNameW(p, 0, o, &n); CloseHandle(p); }
}
static HICON WinIcon(HWND h) {
    DWORD_PTR r = 0; HICON i;
    if (!SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &r) || !r)
        SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &r);
    i = (HICON)r;
    if (!i) i = (HICON)GetClassLongPtrW(h, GCLP_HICON);
    if (!i) i = (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
    if (!i) i = LoadIconW(NULL, IDI_APPLICATION);
    return i;
}
static int IsTaskWin(HWND h) {
    LONG_PTR ex;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || OwnWin(h)) return 0;
    ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return 0;
    if (ex & WS_EX_NOACTIVATE) return 0;
    if (BadClass(h)) return 0;
    return GetWindowTextLengthW(h) > 0;
}

static Task g_t[MAXTASK], g_tf[MAXTASK];
static int g_nt, g_nf;
static RECT g_tr[MAXTASK];
static HWND g_ew[MAXTASK]; static wchar_t g_ex[MAXTASK][MAX_PATH]; static int g_nw;
static HWND g_lastFg;

static BOOL CALLBACK EnumTask(HWND h, LPARAM l) {
    (void)l;
    if (g_nw < MAXTASK && IsTaskWin(h)) { g_ew[g_nw] = h; ExeOf(h, g_ex[g_nw]); g_nw++; }
    return TRUE;
}
static void RefreshTasks(int force) {
    int i, j, used[MAXTASK] = {0}, diff = force;
    g_nw = 0; g_nf = 0;
    EnumWindows(EnumTask, 0);
    for (i = 0; i < g_np && g_nf < MAXTASK; i++) {
        Task *t = &g_tf[g_nf]; ZeroMemory(t, sizeof *t); t->pin = i; t->icon = g_p[i].ic;
        for (j = 0; j < g_nw; j++)
            if (!used[j] && g_ex[j][0] && !_wcsicmp(g_ex[j], g_p[i].target)) {
                used[j] = 1; t->hwnd = g_ew[j]; GetWindowTextW(g_ew[j], t->title, 96); break;
            }
        g_nf++;
    }
    for (j = 0; j < g_nw && g_nf < MAXTASK; j++) if (!used[j]) {
        Task *t = &g_tf[g_nf++]; ZeroMemory(t, sizeof *t);
        t->hwnd = g_ew[j]; t->pin = -1; GetWindowTextW(g_ew[j], t->title, 96); t->icon = WinIcon(g_ew[j]);
    }
    if (g_nf != g_nt) diff = 1;
    for (i = 0; !diff && i < g_nf; i++)
        if (g_t[i].hwnd != g_tf[i].hwnd || g_t[i].pin != g_tf[i].pin || wcscmp(g_t[i].title, g_tf[i].title)) diff = 1;
    if (GetForegroundWindow() != g_lastFg) { g_lastFg = GetForegroundWindow(); diff = 1; }
    if (diff) {
        memcpy(g_t, g_tf, sizeof(Task) * g_nf); g_nt = g_nf;
        Layout(); InvalidateRect(g_tb, NULL, FALSE);
    }
}

/* ================= Win7 titlebar skinner ================= */
typedef struct { HWND tgt, deco; LONG_PTR style; int maxed, minb, maxb, hot, down, active, lastW; RECT last, rest; wchar_t title[128]; } Deco;
static Deco g_d[MAXDECO]; static int g_nd;
static int g_drag; static POINT g_dragPt; static RECT g_dragRc;

static Deco *DecoOfDeco(HWND h) { int i; for (i = 0; i < g_nd; i++) if (g_d[i].deco == h) return &g_d[i]; return NULL; }
static Deco *DecoOfTgt(HWND h) { int i; for (i = 0; i < g_nd; i++) if (g_d[i].tgt == h) return &g_d[i]; return NULL; }

static int ShouldSkin(HWND h) {
    LONG_PTR st, ex; RECT r;
    if (!IsWindowVisible(h) || OwnWin(h) || IsIconic(h)) return 0;
    st = GetWindowLongPtrW(h, GWL_STYLE); ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((st & WS_CAPTION) != WS_CAPTION || (st & WS_CHILD)) return 0;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return 0;
    if (BadClass(h) || GetWindowTextLengthW(h) == 0) return 0;
    GetWindowRect(h, &r);
    return (r.right - r.left >= 80 && r.bottom - r.top >= 40);
}
static void DecoMax(Deco *d) {
    RECT wa; if (d->maxed) return;
    GetWindowRect(d->tgt, &d->rest); GetWA(&wa); d->maxed = 1;
    SetWindowPos(d->tgt, NULL, wa.left, wa.top + CAPH, wa.right - wa.left, wa.bottom - wa.top - CAPH, SWP_NOZORDER | SWP_NOACTIVATE);
}
static void DecoRestore(Deco *d) {
    if (!d->maxed) return; d->maxed = 0;
    SetWindowPos(d->tgt, NULL, d->rest.left, d->rest.top, d->rest.right - d->rest.left, d->rest.bottom - d->rest.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
static void Skin(HWND t) {
    Deco *d; LONG_PTR st, ex, ns; RECT cr, r, wa; POINT o = {0, 0}; int zoomed;
    if (g_nd >= MAXDECO) return;
    zoomed = IsZoomed(t); if (zoomed) ShowWindow(t, SW_RESTORE);
    st = GetWindowLongPtrW(t, GWL_STYLE); ex = GetWindowLongPtrW(t, GWL_EXSTYLE);
    GetClientRect(t, &cr); ClientToScreen(t, &o);
    ns = st & ~(LONG_PTR)WS_CAPTION; if (!(st & WS_THICKFRAME)) ns |= WS_BORDER;
    SetWindowLongPtrW(t, GWL_STYLE, ns);
    r.left = 0; r.top = 0; r.right = cr.right; r.bottom = cr.bottom;
    AdjustWindowRectEx(&r, (DWORD)ns, GetMenu(t) != NULL, (DWORD)ex);
    GetWA(&wa);
    { int x = o.x + r.left, y = o.y + r.top; if (y - CAPH < wa.top) y = wa.top + CAPH;
      SetWindowPos(t, NULL, x, y, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED); }
    d = &g_d[g_nd++]; ZeroMemory(d, sizeof *d);
    d->tgt = t; d->style = st; d->minb = (st & WS_MINIMIZEBOX) != 0; d->maxb = (st & WS_MAXIMIZEBOX) != 0;
    d->deco = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WinShellDeco", L"", WS_POPUP, 0, 0, 100, CAPH, NULL, NULL, g_hi, NULL);
    if (zoomed) DecoMax(d);
}
static void Unskin(Deco *d) {
    if (IsWindow(d->tgt)) {
        RECT cr, r; POINT o = {0, 0}; LONG_PTR ex = GetWindowLongPtrW(d->tgt, GWL_EXSTYLE);
        GetClientRect(d->tgt, &cr); ClientToScreen(d->tgt, &o);
        SetWindowLongPtrW(d->tgt, GWL_STYLE, d->style);
        r.left = 0; r.top = 0; r.right = cr.right; r.bottom = cr.bottom;
        AdjustWindowRectEx(&r, (DWORD)d->style, GetMenu(d->tgt) != NULL, (DWORD)ex);
        SetWindowPos(d->tgt, NULL, o.x + r.left, o.y + r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    DestroyWindow(d->deco);
    *d = g_d[g_nd - 1]; g_nd--;
}
static void UnskinAll(void) { while (g_nd > 0) Unskin(&g_d[g_nd - 1]); }

static BOOL CALLBACK EnumSkin(HWND h, LPARAM l) { (void)l; if (!DecoOfTgt(h) && ShouldSkin(h)) Skin(h); return TRUE; }
static void SyncDecos(void) {
    int i; HWND fg = GetForegroundWindow();
    for (i = g_nd - 1; i >= 0; i--) if (!IsWindow(g_d[i].tgt)) Unskin(&g_d[i]);
    EnumWindows(EnumSkin, 0);
    for (i = 0; i < g_nd; i++) {
        Deco *d = &g_d[i]; RECT r; wchar_t t[128]; int act, vis, redraw = 0; HWND prev; LONG_PTR ex;
        vis = IsWindowVisible(d->tgt) && !IsIconic(d->tgt);
        if (!vis) { if (IsWindowVisible(d->deco)) ShowWindow(d->deco, SW_HIDE); continue; }
        if (IsZoomed(d->tgt)) { ShowWindow(d->tgt, SW_RESTORE); DecoMax(d); }
        GetWindowRect(d->tgt, &r);
        GetWindowTextW(d->tgt, t, 128); if (wcscmp(t, d->title)) { lstrcpynW(d->title, t, 128); redraw = 1; }
        act = (fg == d->tgt); if (act != d->active) { d->active = act; redraw = 1; }
        ex = GetWindowLongPtrW(d->tgt, GWL_EXSTYLE);
        prev = GetWindow(d->tgt, GW_HWNDPREV);
        {
            int w = r.right - r.left, moved = (r.left != d->last.left || r.top != d->last.top || w != d->lastW);
            HWND after = (ex & WS_EX_TOPMOST) ? (prev ? prev : HWND_TOPMOST) : (prev ? prev : HWND_TOP);
            if (moved || prev != d->deco || !IsWindowVisible(d->deco)) {
                if (prev != d->deco || moved || !IsWindowVisible(d->deco))
                    SetWindowPos(d->deco, after, r.left, r.top - CAPH, w, CAPH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (w != d->lastW) {
                    HRGN rg = CreateRoundRectRgn(0, 0, w + 1, CAPH + 12, 10, 10);
                    SetWindowRgn(d->deco, rg, TRUE);
                }
                d->last = r; d->lastW = w; redraw = 1;
            }
        }
        if (redraw) InvalidateRect(d->deco, NULL, FALSE);
    }
}
static int BtnRect(Deco *d, int W, int k, RECT *r) {
    int cr = W - 6, x = cr - 46;
    if (k == 3) { SetRect(r, cr - 46, 1, cr, 21); return 1; }
    if (k == 2) { if (!d->maxb) return 0; SetRect(r, x - 28, 1, x, 21); return 1; }
    if (k == 1) { if (!d->minb) return 0; if (d->maxb) x -= 28; SetRect(r, x - 28, 1, x, 21); return 1; }
    return 0;
}
static int DecoHit(Deco *d, int W, int x, int y) {
    int k; RECT r; POINT p; p.x = x; p.y = y;
    for (k = 1; k <= 3; k++) if (BtnRect(d, W, k, &r) && PtInRect(&r, p)) return k;
    return 0;
}
static void Glyph(HDC m, RECT r, int k, int maxed, COLORREF c) {
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    HPEN p = CreatePen(PS_SOLID, 2, c); HGDIOBJ op = SelectObject(m, p), ob = SelectObject(m, GetStockObject(NULL_BRUSH));
    if (k == 1) { MoveToEx(m, cx - 5, cy + 4, NULL); LineTo(m, cx + 5, cy + 4); }
    else if (k == 2) {
        if (maxed) { Rectangle(m, cx - 3, cy - 5, cx + 5, cy + 2); Rectangle(m, cx - 5, cy - 3, cx + 3, cy + 4); }
        else Rectangle(m, cx - 5, cy - 5, cx + 5, cy + 4);
    } else { MoveToEx(m, cx - 5, cy - 4, NULL); LineTo(m, cx + 5, cy + 5); MoveToEx(m, cx + 4, cy - 4, NULL); LineTo(m, cx - 5, cy + 5); }
    SelectObject(m, op); SelectObject(m, ob); DeleteObject(p);
}
static void PaintDeco(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT c, tr, br; Deco *d = DecoOfDeco(h); HDC m; HBITMAP bm; HGDIOBJ ob; int k, ox, oy;
    GetClientRect(h, &c);
    m = CreateCompatibleDC(dc); bm = CreateCompatibleBitmap(dc, c.right, c.bottom); ob = SelectObject(m, bm);
    if (d) {
        VGrad(m, &c, d->active ? RGB(204, 223, 244) : RGB(226, 232, 240), d->active ? RGB(158, 190, 228) : RGB(206, 215, 227));
        { RECT hl = { 2, 1, c.right - 2, 2 }; FillR(m, &hl, RGB(240, 247, 255)); }
        { HPEN p = CreatePen(PS_SOLID, 1, RGB(58, 74, 100)); HGDIOBJ op = SelectObject(m, p);
          MoveToEx(m, 0, c.bottom, NULL); LineTo(m, 0, 8); LineTo(m, 8, 0); LineTo(m, c.right - 9, 0); LineTo(m, c.right - 1, 8); LineTo(m, c.right - 1, c.bottom);
          SelectObject(m, op); DeleteObject(p); }
        DrawIconEx(m, 8, 7, WinIcon(d->tgt), 16, 16, 0, NULL, DI_NORMAL);
        BtnRect(d, c.right, d->maxb ? 2 : 3, &br);
        if (d->minb) BtnRect(d, c.right, 1, &br); else if (d->maxb) BtnRect(d, c.right, 2, &br); else BtnRect(d, c.right, 3, &br);
        tr.left = 30; tr.top = 0; tr.right = br.left - 6; tr.bottom = c.bottom;
        for (oy = -1; oy <= 1; oy++) for (ox = -1; ox <= 1; ox++) if (ox || oy) {
            RECT g = tr; OffsetRect(&g, ox, oy); Txt(m, d->title, g, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, RGB(232, 242, 255), g_fb);
        }
        Txt(m, d->title, tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, d->active ? RGB(0, 0, 0) : RGB(90, 96, 108), g_fb);
        for (k = 1; k <= 3; k++) if (BtnRect(d, c.right, k, &br)) {
            int hot = (d->hot == k), dn = (d->down == k); COLORREF f, g;
            if (k == 3) { f = dn ? RGB(160, 30, 24) : hot ? RGB(222, 82, 66) : RGB(226, 233, 243); g = RGB(90, 100, 120); }
            else { f = dn ? RGB(196, 214, 238) : hot ? RGB(244, 249, 255) : RGB(226, 233, 243); g = RGB(90, 100, 120); }
            RRect(m, br, f, g, 5);
            Glyph(m, br, k, d->maxed, (k == 3 && (hot || dn)) ? RGB(255, 255, 255) : RGB(36, 48, 72));
        }
    }
    BitBlt(dc, 0, 0, c.right, c.bottom, m, 0, 0, SRCCOPY);
    SelectObject(m, ob); DeleteObject(bm); DeleteDC(m); EndPaint(h, &ps);
}
static LRESULT CALLBACK DecoProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Deco *d = DecoOfDeco(h);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintDeco(h); return 0;
    case WM_LBUTTONDBLCLK:
        if (d && d->maxb && !DecoHit(d, d->lastW, GET_X_LPARAM(l), GET_Y_LPARAM(l))) { if (d->maxed) DecoRestore(d); else DecoMax(d); }
        return 0;
    case WM_LBUTTONDOWN:
        if (!d) return 0;
        SetForegroundWindow(d->tgt);
        d->down = DecoHit(d, d->lastW, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        SetCapture(h);
        if (!d->down) { g_drag = 1; GetCursorPos(&g_dragPt); GetWindowRect(d->tgt, &g_dragRc); }
        InvalidateRect(h, NULL, FALSE); return 0;
    case WM_MOUSEMOVE: {
        POINT p; int hot;
        if (!d) return 0;
        GetCursorPos(&p);
        if (g_drag) {
            RECT wa; int nx, ny; GetWA(&wa);
            if (d->maxed) {
                int rw = d->rest.right - d->rest.left; DecoRestore(d);
                SetWindowPos(d->tgt, NULL, p.x - rw / 2, p.y - CAPH / 2 + CAPH, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                GetWindowRect(d->tgt, &g_dragRc); g_dragPt = p;
            }
            nx = g_dragRc.left + p.x - g_dragPt.x; ny = g_dragRc.top + p.y - g_dragPt.y;
            if (ny - CAPH < wa.top) ny = wa.top + CAPH;
            SetWindowPos(d->tgt, NULL, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
            hot = DecoHit(d, d->lastW, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (hot != d->hot) { d->hot = hot; InvalidateRect(h, NULL, FALSE); }
        }
        return 0; }
    case WM_MOUSELEAVE: if (d) { d->hot = 0; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_LBUTTONUP: {
        int k; if (!d) return 0;
        ReleaseCapture(); g_drag = 0;
        k = DecoHit(d, d->lastW, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (d->down && k == d->down) {
            int a = d->down; d->down = 0;
            if (a == 1) ShowWindow(d->tgt, SW_MINIMIZE);
            else if (a == 2) { if (d->maxed) DecoRestore(d); else DecoMax(d); }
            else PostMessageW(d->tgt, WM_SYSCOMMAND, SC_CLOSE, 0);
        }
        d->down = 0; InvalidateRect(h, NULL, FALSE); return 0; }
    case WM_RBUTTONUP: {
        HMENU m = CreatePopupMenu(); POINT p; int c; if (!d) return 0;
        GetCursorPos(&p);
        AppendMenuW(m, MF_STRING | (d->maxed ? 0 : MF_GRAYED), 1, L"Restore");
        AppendMenuW(m, MF_STRING | (d->minb ? 0 : MF_GRAYED), 2, L"Minimize");
        AppendMenuW(m, MF_STRING | (d->maxb ? 0 : MF_GRAYED), 3, L"Maximize");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, 4, L"Close");
        c = TrackPop(h, m, p.x, p.y, TPM_TOPALIGN);
        if (c == 1) DecoRestore(d); else if (c == 2) ShowWindow(d->tgt, SW_MINIMIZE);
        else if (c == 3) DecoMax(d); else if (c == 4) PostMessageW(d->tgt, WM_SYSCOMMAND, SC_CLOSE, 0);
        return 0; }
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ================= start panel ================= */
typedef struct { wchar_t name[96], path[MAX_PATH]; HICON ic; int tried; } SItem;
static SItem *g_it; static int g_ni, g_cap;
static int *g_vis, g_nvis, g_scroll, g_hotL = -1, g_hotR = -1, g_hotB, g_dn, g_dy, g_ds, g_moved;
static wchar_t g_filter[64];
static DWORD g_lastScan;
static WNDPROC g_oldEdit;
static const wchar_t *RN[] = { L"Documents", L"Pictures", L"Music", L"Computer", L"Control Panel", L"Task Manager", L"Wine Configuration", L"WinShell Settings" };
#define NRN 8

static void AddItem(const wchar_t *name, const wchar_t *path) {
    SItem *s;
    if (StrStrIW(name, L"uninstall")) return;
    if (g_ni >= g_cap) { g_cap = g_cap ? g_cap * 2 : 128; g_it = (SItem *)realloc(g_it, g_cap * sizeof(SItem)); g_vis = (int *)realloc(g_vis, g_cap * sizeof(int)); }
    s = &g_it[g_ni++]; ZeroMemory(s, sizeof *s);
    lstrcpynW(s->name, name, 96); lstrcpynW(s->path, path, MAX_PATH);
}
static void ScanDir(const wchar_t *dir, int depth) {
    wchar_t pat[MAX_PATH], full[MAX_PATH], nm[MAX_PATH], *e; WIN32_FIND_DATAW fd; HANDLE h;
    wsprintfW(pat, L"%s\\*", dir); h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        wsprintfW(full, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) ScanDir(full, depth + 1); continue; }
        e = PathFindExtensionW(fd.cFileName);
        if (!_wcsicmp(e, L".lnk") || !_wcsicmp(e, L".exe") || !_wcsicmp(e, L".url") || !_wcsicmp(e, L".bat")) {
            lstrcpynW(nm, fd.cFileName, MAX_PATH); PathRemoveExtensionW(nm); AddItem(nm, full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static int CmpItem(const void *a, const void *b) { return _wcsicmp(((const SItem *)a)->name, ((const SItem *)b)->name); }
static void Filter(void) {
    int i; g_nvis = 0;
    for (i = 0; i < g_ni; i++) if (!g_filter[0] || StrStrIW(g_it[i].name, g_filter)) g_vis[g_nvis++] = i;
    g_scroll = 0;
}
static void ScanStart(void) {
    static const int ids[] = { CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS, CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY };
    wchar_t p[MAX_PATH]; int i, j;
    for (i = 0; i < g_ni; i++) if (g_it[i].ic) DestroyIcon(g_it[i].ic);
    g_ni = 0;
    for (i = 0; i < 4; i++) if (SUCCEEDED(SHGetFolderPathW(NULL, ids[i], NULL, 0, p))) ScanDir(p, 0);
    if (g_ni) qsort(g_it, g_ni, sizeof(SItem), CmpItem);
    for (i = 1, j = 0; i < g_ni; i++) {   /* drop duplicate names */
        if (!_wcsicmp(g_it[i].name, g_it[j].name)) continue;
        g_it[++j] = g_it[i];
    }
    g_ni = g_ni ? j + 1 : 0;
    Filter(); g_lastScan = GetTickCount();
}
static void ItemIcon(SItem *s) {
    SHFILEINFOW fi;
    if (s->tried) return; s->tried = 1; ZeroMemory(&fi, sizeof fi);
    if (SHGetFileInfoW(s->path, 0, &fi, sizeof fi, SHGFI_ICON | SHGFI_LARGEICON)) s->ic = fi.hIcon;
}
static void RightRect(int i, RECT *r) { SetRect(r, LW + 8, 122 + i * 30, PW - 10, 122 + i * 30 + 28); }
static int StartHit(int x, int y, int *idx) {
    RECT r; POINT p; int i; p.x = x; p.y = y;
    if (x < LW) {
        if (y >= LISTY && y < LISTY + LISTH) { i = (y - LISTY + g_scroll) / IH; if (i >= 0 && i < g_nvis) { *idx = i; return 1; } }
        return 0;
    }
    SetRect(&r, LW + (PW - LW - 64) / 2, 14, LW + (PW - LW - 64) / 2 + 64, 78);
    if (PtInRect(&r, p)) return 3;
    SetRect(&r, LW + 10, PH - 52, PW - 10, PH - 24);
    if (PtInRect(&r, p)) return 4;
    for (i = 0; i < NRN; i++) { RightRect(i, &r); if (PtInRect(&r, p)) { *idx = i; return 2; } }
    return 0;
}
static void DrawPic(HDC m, RECT r) {
    RRect(m, r, RGB(255, 255, 255), RGB(210, 225, 245), 6);
    InflateRect(&r, -3, -3);
    if (g_pic) {
        HDC s = CreateCompatibleDC(m); BITMAP bm; HGDIOBJ ob = SelectObject(s, g_pic);
        GetObject(g_pic, sizeof bm, &bm); SetStretchBltMode(m, HALFTONE);
        StretchBlt(m, r.left, r.top, r.right - r.left, r.bottom - r.top, s, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(s, ob); DeleteDC(s);
    } else {
        int cx = (r.left + r.right) / 2, w = r.right - r.left; HBRUSH b = CreateSolidBrush(RGB(255, 255, 255)); HGDIOBJ ob;
        VGrad(m, &r, RGB(120, 170, 225), RGB(40, 90, 160));
        ob = SelectObject(m, b); SelectObject(m, GetStockObject(NULL_PEN));
        Ellipse(m, cx - w / 6, r.top + w / 6, cx + w / 6, r.top + w / 6 + w / 3);
        Ellipse(m, cx - w / 3, r.top + w / 2, cx + w / 3, r.bottom + w / 4);
        SelectObject(m, ob); DeleteObject(b);
    }
}
static void PaintStart(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps), m = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, PW, PH);
    HGDIOBJ ob = SelectObject(m, bm); RECT all = { 0, 0, PW, PH }, lp = { 1, 1, LW, PH - 1 }, rp = { LW, 1, PW - 1, PH - 1 }, r; int i;
    FillR(m, &all, RGB(52, 84, 130)); FillR(m, &lp, RGB(255, 255, 255)); VGrad(m, &rp, RGB(58, 94, 146), RGB(22, 42, 76));
    SaveDC(m); IntersectClipRect(m, 0, LISTY, LW, LISTY + LISTH);
    for (i = 0; i < g_nvis; i++) {
        int y = LISTY + i * IH - g_scroll; SItem *it; RECT tr;
        if (y + IH < LISTY || y > LISTY + LISTH) continue;
        it = &g_it[g_vis[i]]; ItemIcon(it);
        SetRect(&r, 4, y, LW - 14, y + IH);
        if (i == g_hotL) RRect(m, r, RGB(222, 236, 252), RGB(170, 200, 235), 4);
        DrawIconEx(m, 12, y + (IH - 24) / 2, it->ic ? it->ic : LoadIconW(NULL, IDI_APPLICATION), 24, 24, 0, NULL, DI_NORMAL);
        SetRect(&tr, 46, y, LW - 18, y + IH); Txt(m, it->name, tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, RGB(20, 30, 50), g_f);
    }
    RestoreDC(m, -1);
    if (!g_nvis) { SetRect(&r, 12, 20, LW - 12, 50); Txt(m, L"No matching programs.", r, DT_SINGLELINE, RGB(90, 90, 90), g_f); }
    if (g_nvis * IH > LISTH) {
        int tot = g_nvis * IH, bh = LISTH * LISTH / tot, by = LISTY + g_scroll * (LISTH - bh) / (tot - LISTH);
        SetRect(&r, LW - 9, by, LW - 4, by + bh); RRect(m, r, RGB(180, 190, 205), RGB(160, 170, 190), 4);
    }
    SetRect(&r, 8, PH - 78, LW - 8, PH - 77); FillR(m, &r, RGB(205, 215, 230));
    SetRect(&r, 14, PH - 74, LW - 8, PH - 56); Txt(m, L"Search programs", r, DT_SINGLELINE, RGB(110, 120, 140), g_f);
    /* right pane */
    SetRect(&r, LW + (PW - LW - 64) / 2, 14, LW + (PW - LW - 64) / 2 + 64, 78); DrawPic(m, r);
    SetRect(&r, LW + 4, 84, PW - 6, 110); Txt(m, g_c.user, r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS, RGB(255, 255, 255), g_fb);
    for (i = 0; i < NRN; i++) {
        RightRect(i, &r);
        if (i == g_hotR) RRect(m, r, RGB(92, 132, 190), RGB(150, 185, 230), 4);
        r.left += 10; Txt(m, RN[i], r, DT_SINGLELINE | DT_VCENTER, RGB(255, 255, 255), g_f);
    }
    SetRect(&r, LW + 10, PH - 52, PW - 10, PH - 24);
    RRect(m, r, g_hotB ? RGB(70, 110, 170) : RGB(36, 62, 104), RGB(140, 170, 215), 6);
    Txt(m, L"Shut down  \x25B8", r, DT_SINGLELINE | DT_CENTER | DT_VCENTER, RGB(255, 255, 255), g_fb);
    BitBlt(dc, 0, 0, PW, PH, m, 0, 0, SRCCOPY);
    SelectObject(m, ob); DeleteObject(bm); DeleteDC(m); EndPaint(h, &ps);
}
static void ChangePic(void) {
    wchar_t f[MAX_PATH];
    HideStart(); g_inMenu = 1;
    if (PickFile(NULL, f, IMG_FILTER, L"Choose your account picture")) {
        lstrcpynW(g_c.pic, f, MAX_PATH); SaveCfg();
        if (g_pic) DeleteObject(g_pic);
        g_pic = LoadImg(g_c.pic);
    }
    g_inMenu = 0;
}
static void KnownFolder(int csidl) { wchar_t p[MAX_PATH]; if (SUCCEEDED(SHGetFolderPathW(NULL, csidl, NULL, 0, p))) Launch(p); }
static void ExitShell(int restart) {
    if (restart) { wchar_t me[MAX_PATH]; GetModuleFileNameW(NULL, me, MAX_PATH); ShellExecuteW(NULL, L"open", me, L"/restart", NULL, SW_SHOWNORMAL); }
    DestroyWindow(g_tb);
}
static void StartClick(int x, int y) {
    int idx = 0, k = StartHit(x, y, &idx);
    if (k == 1) { Launch(g_it[g_vis[idx]].path); HideStart(); }
    else if (k == 2) {
        HideStart();
        switch (idx) {
        case 0: KnownFolder(CSIDL_PERSONAL); break;
        case 1: KnownFolder(CSIDL_MYPICTURES); break;
        case 2: KnownFolder(CSIDL_MYMUSIC); break;
        case 3: Run(L"explorer.exe", NULL); break;
        case 4: Run(L"control.exe", NULL); break;
        case 5: Run(L"taskmgr.exe", NULL); break;
        case 6: Run(L"winecfg.exe", NULL); break;
        case 7: OpenSettings(0); break;
        }
    }
    else if (k == 3) ChangePic();
    else if (k == 4) {
        HMENU m = CreatePopupMenu(); POINT p = { LW + 10, PH - 52 }; int c;
        AppendMenuW(m, MF_STRING, 1, L"Shut down (wineboot -s)");
        AppendMenuW(m, MF_STRING, 2, L"Restart shell");
        AppendMenuW(m, MF_STRING, 3, L"Log off (exit shell)");
        ClientToScreen(g_start, &p);
        c = TrackPop(g_start, m, p.x, p.y, TPM_BOTTOMALIGN);
        if (c == 1) { HideStart(); Run(L"wineboot.exe", L"-s"); }
        else if (c == 2) ExitShell(1); else if (c == 3) ExitShell(0);
    }
}
static LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN) { if (g_nvis) { Launch(g_it[g_vis[0]].path); HideStart(); } return 0; }
        if (w == VK_ESCAPE) { HideStart(); return 0; }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    return CallWindowProcW(g_oldEdit, h, m, w, l);
}
static void ScrollClamp(void) {
    int mx = g_nvis * IH - LISTH; if (mx < 0) mx = 0; g_scroll = Clamp(g_scroll, 0, mx);
}
static LRESULT CALLBACK StartProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, PH - 54, LW - 24, 26, h, (HMENU)(INT_PTR)IDC_SEARCH, g_hi, NULL);
        SendMessageW(g_search, WM_SETFONT, (WPARAM)g_f, TRUE);
        g_oldEdit = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC, (LONG_PTR)EditSub);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintStart(h); return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_SEARCH && HIWORD(w) == EN_CHANGE) { GetWindowTextW(g_search, g_filter, 64); Filter(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && !g_inMenu) HideStart(); return 0;
    case WM_MOUSEWHEEL: g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120 * IH * 2; ScrollClamp(); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: g_dn = 1; g_dy = GET_Y_LPARAM(l); g_ds = g_scroll; g_moved = 0; SetCapture(h); return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), idx = -1, k, nl = -1, nr = -1, nb;
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        if (g_dn && x < LW) {
            int dy = y - g_dy; if (abs(dy) > 6) g_moved = 1;
            if (g_moved) { g_scroll = g_ds - dy; ScrollClamp(); }
        }
        k = StartHit(x, y, &idx);
        if (k == 1) nl = idx; else if (k == 2) nr = idx; nb = (k == 4);
        if (nl != g_hotL || nr != g_hotR || nb != g_hotB || g_moved) { g_hotL = nl; g_hotR = nr; g_hotB = nb; InvalidateRect(h, NULL, FALSE); }
        return 0; }
    case WM_MOUSELEAVE: g_hotL = g_hotR = -1; g_hotB = 0; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONUP:
        if (g_dn) { g_dn = 0; ReleaseCapture(); if (!g_moved) StartClick(GET_X_LPARAM(l), GET_Y_LPARAM(l)); }
        return 0;
    case WM_RBUTTONUP: {
        int idx = 0; POINT p; p.x = GET_X_LPARAM(l); p.y = GET_Y_LPARAM(l);
        if (StartHit(p.x, p.y, &idx) == 1) {
            HMENU m = CreatePopupMenu(); int c; SItem *it = &g_it[g_vis[idx]];
            AppendMenuW(m, MF_STRING, 1, L"Open"); AppendMenuW(m, MF_STRING, 2, L"Pin to taskbar");
            ClientToScreen(h, &p); c = TrackPop(h, m, p.x, p.y, TPM_TOPALIGN);
            if (c == 1) { Launch(it->path); HideStart(); }
            else if (c == 2) { AddPin(it->path); SavePins(); RefreshTasks(1); }
        }
        return 0; }
    }
    return DefWindowProcW(h, msg, w, l);
}
static void ShowStart(void) {
    int x, y;
    if (GetTickCount() - g_lastScan > 20000) ScanStart();
    x = Clamp(g_tbRect.left, 0, g_scrW - PW);
    y = g_c.tbTop ? g_tbRect.bottom : g_tbRect.top - PH;
    y = Clamp(y, 0, g_scrH - PH);
    g_hotL = g_hotR = -1;
    SetWindowPos(g_start, HWND_TOPMOST, x, y, PW, PH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_start); SetFocus(g_search);
    InvalidateRect(g_tb, NULL, FALSE);
}
static void HideStart(void) {
    if (!g_start || !IsWindowVisible(g_start)) return;
    ShowWindow(g_start, SW_HIDE); g_startHide = GetTickCount();
    SetWindowTextW(g_search, L"");
    InvalidateRect(g_tb, NULL, FALSE);
}

/* ================= desktop (full desktop mode) ================= */
typedef struct { wchar_t name[64], path[MAX_PATH]; HICON ic; int x, y; } DItem;
static DItem g_di[256]; static int g_ndi, g_dsel = -1;
#define CW 86
#define CH 96

static void DeskScanDir(const wchar_t *dir) {
    wchar_t pat[MAX_PATH]; WIN32_FIND_DATAW fd; HANDLE h;
    wsprintfW(pat, L"%s\\*", dir); h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DItem *d; SHFILEINFOW fi; wchar_t *e;
        if (g_ndi >= 256 || fd.cFileName[0] == L'.' || !_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
        d = &g_di[g_ndi++]; ZeroMemory(d, sizeof *d);
        lstrcpynW(d->name, fd.cFileName, 64); wsprintfW(d->path, L"%s\\%s", dir, fd.cFileName);
        e = PathFindExtensionW(d->name); if (!_wcsicmp(e, L".lnk") || !_wcsicmp(e, L".url")) *e = 0;
        ZeroMemory(&fi, sizeof fi);
        if (SHGetFileInfoW(d->path, 0, &fi, sizeof fi, SHGFI_ICON | SHGFI_LARGEICON)) d->ic = fi.hIcon;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void DesktopReload(void) {
    wchar_t p[MAX_PATH]; int i, x = 8, y = 8; RECT wa;
    for (i = 0; i < g_ndi; i++) if (g_di[i].ic) DestroyIcon(g_di[i].ic);
    g_ndi = 0; g_dsel = -1;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, p))) DeskScanDir(p);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, p))) DeskScanDir(p);
    GetWA(&wa);
    for (i = 0; i < g_ndi; i++) {
        if (y + CH > wa.bottom) { y = wa.top + 8; x += CW; }
        if (y < wa.top + 8) y = wa.top + 8;
        g_di[i].x = x; g_di[i].y = y; y += CH;
    }
    if (g_wall) DeleteObject(g_wall);
    g_wall = LoadImg(g_c.wall);
}
static int DeskHit(int x, int y) {
    int i; for (i = 0; i < g_ndi; i++) if (x >= g_di[i].x && x < g_di[i].x + CW && y >= g_di[i].y && y < g_di[i].y + CH) return i;
    return -1;
}
static void PaintDesk(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps), m = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, g_scrW, g_scrH);
    HGDIOBJ ob = SelectObject(m, bm); RECT all = { 0, 0, g_scrW, g_scrH }; int i;
    if (g_wall) {
        HDC s = CreateCompatibleDC(m); BITMAP b; double sc, sx, sy; int sw, sh; HGDIOBJ o2 = SelectObject(s, g_wall);
        GetObject(g_wall, sizeof b, &b); sx = (double)g_scrW / b.bmWidth; sy = (double)g_scrH / b.bmHeight; sc = sx > sy ? sx : sy;
        sw = (int)(g_scrW / sc); sh = (int)(g_scrH / sc); SetStretchBltMode(m, HALFTONE);
        StretchBlt(m, 0, 0, g_scrW, g_scrH, s, (b.bmWidth - sw) / 2, (b.bmHeight - sh) / 2, sw, sh, SRCCOPY);
        SelectObject(s, o2); DeleteDC(s);
    } else VGrad(m, &all, RGB(24, 96, 170), RGB(6, 30, 80));
    for (i = 0; i < g_ndi; i++) {
        DItem *d = &g_di[i]; RECT cell = { d->x, d->y, d->x + CW, d->y + CH }, tr;
        if (i == g_dsel) RRect(m, cell, RGB(70, 120, 190), RGB(170, 205, 245), 5);
        DrawIconEx(m, d->x + (CW - 32) / 2, d->y + 8, d->ic ? d->ic : LoadIconW(NULL, IDI_APPLICATION), 32, 32, 0, NULL, DI_NORMAL);
        tr.left = d->x + 3; tr.top = d->y + 46; tr.right = d->x + CW - 3; tr.bottom = d->y + 46 + 34;
        { RECT s = tr; OffsetRect(&s, 1, 1); Txt(m, d->name, s, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS, RGB(0, 0, 0), g_f); }
        Txt(m, d->name, tr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS, RGB(255, 255, 255), g_f);
    }
    BitBlt(dc, 0, 0, g_scrW, g_scrH, m, 0, 0, SRCCOPY);
    SelectObject(m, ob); DeleteObject(bm); DeleteDC(m); EndPaint(h, &ps);
}
static LRESULT CALLBACK DeskProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintDesk(h); return 0;
    case WM_LBUTTONDOWN: { int i = DeskHit(GET_X_LPARAM(l), GET_Y_LPARAM(l)); HideStart(); if (i != g_dsel) { g_dsel = i; InvalidateRect(h, NULL, FALSE); } return 0; }
    case WM_LBUTTONDBLCLK: { int i = DeskHit(GET_X_LPARAM(l), GET_Y_LPARAM(l)); if (i >= 0) Launch(g_di[i].path); return 0; }
    case WM_RBUTTONUP: {
        POINT p; HMENU m = CreatePopupMenu(); int i, c; p.x = GET_X_LPARAM(l); p.y = GET_Y_LPARAM(l);
        i = DeskHit(p.x, p.y); g_dsel = i; InvalidateRect(h, NULL, FALSE); ClientToScreen(h, &p);
        if (i >= 0) { AppendMenuW(m, MF_STRING, 1, L"Open"); AppendMenuW(m, MF_SEPARATOR, 0, NULL); }
        AppendMenuW(m, MF_STRING, 2, L"Refresh"); AppendMenuW(m, MF_STRING, 3, L"Change wallpaper...");
        AppendMenuW(m, MF_STRING, 4, L"WinShell settings");
        c = TrackPop(h, m, p.x, p.y, TPM_TOPALIGN);
        if (c == 1) Launch(g_di[i].path);
        else if (c == 2) { DesktopReload(); InvalidateRect(h, NULL, FALSE); }
        else if (c == 3) { wchar_t f[MAX_PATH]; if (PickFile(NULL, f, IMG_FILTER, L"Choose wallpaper")) { lstrcpynW(g_c.wall, f, MAX_PATH); SaveCfg(); DesktopReload(); InvalidateRect(h, NULL, FALSE); } }
        else if (c == 4) OpenSettings(2);
        return 0; }
    }
    return DefWindowProcW(h, msg, w, l);
}
static void ShowDesk(void) {
    if (!g_desk) return;
    DesktopReload();
    SetWindowPos(g_desk, HWND_BOTTOM, 0, 0, g_scrW, g_scrH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_desk, NULL, FALSE);
}

/* ================= clock ================= */
static void NowTime(SYSTEMTIME *st) {
    if (g_c.tzFixed) {
        FILETIME ft; ULARGE_INTEGER u; GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        u.QuadPart += (ULONGLONG)((LONGLONG)g_c.tzOff * 60 * 10000000LL);
        ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart; FileTimeToSystemTime(&ft, st);
    } else GetLocalTime(st);
}
static int UpdateClock(void) {
    SYSTEMTIME st; wchar_t t[64], d[64];
    NowTime(&st);
    GetTimeFormatEx(L"en-IN", 0, &st, g_c.timeFmt[0] ? g_c.timeFmt : NULL, t, 64);
    GetDateFormatEx(L"en-IN", 0, &st, NULL, g_c.dateFmt[0] ? g_c.dateFmt : NULL, d, 64, NULL);
    if (!wcscmp(t, g_timeStr) && !wcscmp(d, g_dateStr)) return 0;
    lstrcpynW(g_timeStr, t, 64); lstrcpynW(g_dateStr, d, 64); return 1;
}

/* ================= taskbar ================= */
static RECT g_rOrb, g_rTray, g_rClock, g_rDesk;
static int g_hot = H_NONE;
static int IconSz(void) { return g_c.tbH >= 52 ? 32 : (g_c.tbH >= 38 ? 24 : 16); }

static void Layout(void) {
    RECT c; HDC dc; HGDIOBJ of; SIZE a = {0, 0}, b = {0, 0}; int H, W, cw = 0, x0, x1, i, nT = 0, nI = 0, avail, bw;
    if (!g_tb) return;
    GetClientRect(g_tb, &c); H = c.bottom; W = c.right;
    SetRect(&g_rOrb, 2, 0, 2 + H + 4, H);
    SetRect(&g_rDesk, W - 14, 0, W, H);
    if (g_c.showClock) {
        dc = GetDC(g_tb); of = SelectObject(dc, g_f);
        GetTextExtentPoint32W(dc, g_timeStr, lstrlenW(g_timeStr), &a);
        if (g_c.showDate) GetTextExtentPoint32W(dc, g_dateStr, lstrlenW(g_dateStr), &b);
        SelectObject(dc, of); ReleaseDC(g_tb, dc);
        cw = (g_c.showDate && H < 36) ? a.cx + b.cx + 24 : (a.cx > b.cx ? a.cx : b.cx) + 16;
    }
    SetRect(&g_rClock, g_rDesk.left - cw, 0, g_rDesk.left, H);
    SetRect(&g_rTray, g_rClock.left - H, 0, g_rClock.left, H);
    x0 = g_rOrb.right + 6; x1 = g_rTray.left - 6; avail = x1 - x0;
    for (i = 0; i < g_nt; i++) { if (g_c.titles && g_t[i].hwnd) nT++; else nI++; }
    bw = H;
    if (nT) { bw = (avail - nI * H) / nT; bw = Clamp(bw, H, 170); }
    if (nT * bw + nI * H > avail && g_nt) { int tot = nT * bw + nI * H; (void)tot; bw = Clamp(avail / g_nt, 28, bw); }
    for (i = 0; i < g_nt; i++) {
        int w2 = (g_c.titles && g_t[i].hwnd) ? bw : (nT ? H : bw);
        if (!nT) w2 = Clamp(avail / (g_nt ? g_nt : 1), 28, H);
        SetRect(&g_tr[i], x0, 2, x0 + w2 - 2, H - 2); x0 += w2;
    }
}
static int Hit(int x, int y) {
    POINT p; int i; p.x = x; p.y = y;
    if (PtInRect(&g_rOrb, p)) return H_ORB;
    if (PtInRect(&g_rDesk, p)) return H_DESK;
    if (g_c.showClock && PtInRect(&g_rClock, p)) return H_CLOCK;
    if (PtInRect(&g_rTray, p)) return H_TRAY;
    for (i = 0; i < g_nt; i++) if (PtInRect(&g_tr[i], p)) return i;
    return H_NONE;
}
static void DrawOrb(HDC m, const RECT *r, int hot) {
    int d = (r->bottom - r->top) - 6, cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, s = d / 5; RECT q;
    HBRUSH b = CreateSolidBrush(hot ? RGB(86, 150, 228) : RGB(44, 100, 182)), b2 = CreateSolidBrush(hot ? RGB(140, 195, 245) : RGB(96, 156, 222));
    HPEN p = CreatePen(PS_SOLID, 1, RGB(200, 225, 250)); HGDIOBJ ob = SelectObject(m, b), op = SelectObject(m, p);
    Ellipse(m, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
    SelectObject(m, b2); SelectObject(m, GetStockObject(NULL_PEN));
    Ellipse(m, cx - d / 2 + 3, cy - d / 2 + 3, cx + d / 2 - 3, cy);
    SelectObject(m, ob); SelectObject(m, op); DeleteObject(b); DeleteObject(b2); DeleteObject(p);
    SetRect(&q, cx - s - 1, cy - s - 1, cx - 1, cy - 1); FillR(m, &q, RGB(240, 90, 40));
    SetRect(&q, cx + 1, cy - s - 1, cx + s + 1, cy - 1); FillR(m, &q, RGB(120, 200, 50));
    SetRect(&q, cx - s - 1, cy + 1, cx - 1, cy + s + 1); FillR(m, &q, RGB(50, 140, 230));
    SetRect(&q, cx + 1, cy + 1, cx + s + 1, cy + s + 1); FillR(m, &q, RGB(250, 200, 40));
}
static void PaintTb(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps), m = CreateCompatibleDC(dc); RECT c, t, r; HBITMAP bm; HGDIOBJ ob; int i, is = IconSz(), H; HWND fg = GetForegroundWindow();
    GetClientRect(h, &c); H = c.bottom;
    bm = CreateCompatibleBitmap(dc, c.right, c.bottom); ob = SelectObject(m, bm);
    VGrad(m, &c, RGB(76, 104, 148), RGB(20, 36, 66));
    SetRect(&t, 0, 0, c.right, 1); FillR(m, &t, RGB(175, 200, 235));
    DrawOrb(m, &g_rOrb, g_hot == H_ORB || IsWindowVisible(g_start));
    for (i = 0; i < g_nt; i++) {
        Task *k = &g_t[i]; int act = k->hwnd && (k->hwnd == fg), hot = (g_hot == i), ix, iy; HICON ic;
        r = g_tr[i];
        if (act) RRect(m, r, RGB(122, 152, 198), RGB(210, 228, 252), 6);
        else if (hot) RRect(m, r, RGB(90, 120, 166), RGB(165, 190, 225), 6);
        else if (k->hwnd) RRect(m, r, RGB(52, 78, 118), RGB(92, 118, 158), 6);
        ic = k->icon ? k->icon : LoadIconW(NULL, IDI_APPLICATION);
        if (g_c.titles && k->hwnd) ix = r.left + 8; else ix = (r.left + r.right - is) / 2;
        iy = (r.top + r.bottom - is) / 2;
        DrawIconEx(m, ix, iy, ic, is, is, 0, NULL, DI_NORMAL);
        if (g_c.titles && k->hwnd) { RECT tr; SetRect(&tr, ix + is + 6, r.top, r.right - 6, r.bottom); Txt(m, k->title, tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, RGB(255, 255, 255), g_f); }
        else if (k->hwnd) { RECT u; SetRect(&u, r.left + 6, r.bottom - 4, r.right - 6, r.bottom - 2); FillR(m, &u, RGB(150, 200, 255)); }
    }
    /* app tray button: 3x3 dots */
    if (g_hot == H_TRAY) { r = g_rTray; InflateRect(&r, -2, -4); RRect(m, r, RGB(90, 120, 166), RGB(165, 190, 225), 6); }
    { int cx = (g_rTray.left + g_rTray.right) / 2, cy = H / 2, a, b2, g = Clamp(H / 10, 3, 6);
      for (a = -1; a <= 1; a++) for (b2 = -1; b2 <= 1; b2++) { RECT q; SetRect(&q, cx + a * (g + 3) - g / 2, cy + b2 * (g + 3) - g / 2, cx + a * (g + 3) + (g + 1) / 2, cy + b2 * (g + 3) + (g + 1) / 2); FillR(m, &q, RGB(225, 238, 255)); } }
    if (g_c.showClock) {
        RECT a = g_rClock, b = g_rClock;
        if (g_hot == H_CLOCK) { r = g_rClock; InflateRect(&r, 0, -3); RRect(m, r, RGB(90, 120, 166), RGB(165, 190, 225), 6); }
        if (g_c.showDate && H >= 36) {
            a.bottom = H / 2 + 2; b.top = H / 2 - 2;
            Txt(m, g_timeStr, a, DT_SINGLELINE | DT_CENTER | DT_BOTTOM, RGB(255, 255, 255), g_f);
            Txt(m, g_dateStr, b, DT_SINGLELINE | DT_CENTER | DT_TOP, RGB(255, 255, 255), g_f);
        } else if (g_c.showDate) {
            wchar_t s[140]; wsprintfW(s, L"%s  %s", g_timeStr, g_dateStr);
            Txt(m, s, g_rClock, DT_SINGLELINE | DT_CENTER | DT_VCENTER, RGB(255, 255, 255), g_f);
        } else Txt(m, g_timeStr, g_rClock, DT_SINGLELINE | DT_CENTER | DT_VCENTER, RGB(255, 255, 255), g_f);
    }
    r = g_rDesk; VGrad(m, &r, g_hot == H_DESK ? RGB(160, 200, 245) : RGB(96, 130, 180), g_hot == H_DESK ? RGB(110, 150, 205) : RGB(40, 66, 110));
    { RECT l = { r.left, r.top, r.left + 1, r.bottom }; FillR(m, &l, RGB(190, 210, 240)); }
    BitBlt(dc, 0, 0, c.right, c.bottom, m, 0, 0, SRCCOPY);
    SelectObject(m, ob); DeleteObject(bm); DeleteDC(m); EndPaint(h, &ps);
}
static void PlaceTaskbar(void) {
    int w = g_scrW * g_c.tbW / 100, x = (g_scrW - w) / 2, y = g_c.tbTop ? 0 : g_scrH - g_c.tbH;
    SetRect(&g_tbRect, x, y, x + w, y + g_c.tbH);
    SetWindowPos(g_tb, HWND_TOPMOST, x, y, w, g_c.tbH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_c.autoHide) {
        int len = g_scrW * g_c.revLen / 100, rx = Clamp(g_scrW * g_c.revPos / 100 - len / 2, 0, g_scrW - len), ry = g_c.tbTop ? 0 : g_scrH - g_c.revThk;
        SetWindowPos(g_rev, HWND_TOPMOST, rx, ry, len, g_c.revThk, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_rev, NULL, TRUE);
    } else ShowWindow(g_rev, SW_HIDE);
    { RECT wa; GetWA(&wa); SystemParametersInfoW(SPI_SETWORKAREA, 0, &wa, SPIF_SENDCHANGE); }
    Layout(); InvalidateRect(g_tb, NULL, FALSE);
}
static void ToggleDesktop(void) {
    static HWND hid[MAXTASK]; static int nh; int i;
    if (!g_showDesk) {
        nh = 0;
        for (i = 0; i < g_nt; i++) if (g_t[i].hwnd && !IsIconic(g_t[i].hwnd)) { hid[nh++] = g_t[i].hwnd; ShowWindow(g_t[i].hwnd, SW_MINIMIZE); }
        g_showDesk = 1;
    } else {
        for (i = 0; i < nh; i++) if (IsWindow(hid[i])) ShowWindow(hid[i], SW_RESTORE);
        g_showDesk = 0;
    }
}
static void ClickTask(int i) {
    Task *t = &g_t[i]; HWND fg = GetForegroundWindow();
    if (!t->hwnd) { if (t->pin >= 0) Launch(g_p[t->pin].path); return; }
    if (IsIconic(t->hwnd)) { ShowWindow(t->hwnd, SW_RESTORE); SetForegroundWindow(t->hwnd); }
    else if (fg == t->hwnd) ShowWindow(t->hwnd, SW_MINIMIZE);
    else { BringWindowToTop(t->hwnd); SetForegroundWindow(t->hwnd); }
}
static void TrayMenu(void) {
    HMENU m = CreatePopupMenu(); int c; POINT p = { g_rTray.left, 0 }; int ay = g_c.tbTop ? g_tbRect.bottom : g_tbRect.top;
    ClientToScreen(g_tb, &p);
    AppendMenuW(m, MF_STRING, 1, L"WinShell Settings");
    AppendMenuW(m, MF_STRING, 2, L"Task Manager");
    AppendMenuW(m, MF_STRING, 3, L"Wine Configuration");
    AppendMenuW(m, MF_STRING, 4, L"Control Panel");
    AppendMenuW(m, MF_STRING, 5, L"File Explorer");
    AppendMenuW(m, MF_STRING, 6, L"Registry Editor");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_c.fullDesk ? MF_CHECKED : 0), 7, L"Full desktop mode");
    AppendMenuW(m, MF_STRING | (g_c.skin ? MF_CHECKED : 0), 8, L"Windows 7 title bars");
    AppendMenuW(m, MF_STRING, 9, L"Show desktop");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 10, L"Restart shell");
    AppendMenuW(m, MF_STRING, 11, L"Exit shell");
    c = TrackPop(g_tb, m, p.x, ay, g_c.tbTop ? TPM_TOPALIGN : TPM_BOTTOMALIGN);
    switch (c) {
    case 1: OpenSettings(0); break;
    case 2: Run(L"taskmgr.exe", NULL); break;
    case 3: Run(L"winecfg.exe", NULL); break;
    case 4: Run(L"control.exe", NULL); break;
    case 5: Run(L"explorer.exe", NULL); break;
    case 6: Run(L"regedit.exe", NULL); break;
    case 7: g_c.fullDesk = !g_c.fullDesk; SaveCfg(); ApplyAll(); break;
    case 8: g_c.skin = !g_c.skin; SaveCfg(); ApplyAll(); break;
    case 9: ToggleDesktop(); break;
    case 10: ExitShell(1); break;
    case 11: ExitShell(0); break;
    }
}
static void TbRight(int x, int y) {
    int k = Hit(x, y), c; POINT pt; HMENU m = CreatePopupMenu(); int ay = g_c.tbTop ? g_tbRect.bottom : g_tbRect.top;
    pt.x = x; pt.y = y; ClientToScreen(g_tb, &pt);
    HideStart();
    if (k >= 0 && k < g_nt) {
        Task *t = &g_t[k];
        if (t->hwnd) AppendMenuW(m, MF_STRING, 1, L"Close window");
        if (t->pin >= 0) { AppendMenuW(m, MF_STRING, 2, L"Unpin from taskbar"); if (t->hwnd) AppendMenuW(m, MF_STRING, 3, L"Open new instance"); }
        else AppendMenuW(m, MF_STRING, 2, L"Pin to taskbar");
        c = TrackPop(g_tb, m, pt.x, ay, g_c.tbTop ? TPM_TOPALIGN : TPM_BOTTOMALIGN);
        if (c == 1) PostMessageW(t->hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        else if (c == 2) {
            if (t->pin >= 0) RemovePin(t->pin);
            else { wchar_t exe[MAX_PATH]; ExeOf(t->hwnd, exe); if (exe[0]) { AddPin(exe); SavePins(); RefreshTasks(1); } }
        } else if (c == 3) Launch(g_p[t->pin].path);
    } else {
        AppendMenuW(m, MF_STRING, 1, L"Taskbar settings");
        AppendMenuW(m, MF_STRING, 2, L"Show desktop");
        AppendMenuW(m, MF_STRING, 3, L"Task Manager");
        AppendMenuW(m, MF_STRING, 4, L"Exit shell");
        c = TrackPop(g_tb, m, pt.x, ay, g_c.tbTop ? TPM_TOPALIGN : TPM_BOTTOMALIGN);
        if (c == 1) OpenSettings(0); else if (c == 2) ToggleDesktop(); else if (c == 3) Run(L"taskmgr.exe", NULL); else if (c == 4) ExitShell(0);
    }
}
static void TbTimer(void) {
    static int tick;
    tick++;
    if (g_c.skin) SyncDecos();
    if (tick % 5 == 0) RefreshTasks(0);
    if (tick % 10 == 0) {
        if (UpdateClock()) { Layout(); InvalidateRect(g_tb, NULL, FALSE); }
        if (IsWindowVisible(g_tb)) SetWindowPos(g_tb, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (IsWindowVisible(g_start) && !g_inMenu && GetForegroundWindow() != g_start && GetTickCount() - g_startHide > 300) {
        HWND fg = GetForegroundWindow(); if (fg && fg != g_tb && !GetParent(fg)) HideStart();
    }
    if (g_c.autoHide && IsWindowVisible(g_tb) && !g_inMenu && !IsWindowVisible(g_start)) {
        POINT p; GetCursorPos(&p);
        if (PtInRect(&g_tbRect, p)) g_lastIn = GetTickCount();
        else if (GetTickCount() - g_lastIn > 1500) ShowWindow(g_tb, SW_HIDE);
    }
}
static LRESULT CALLBACK TbProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: SetTimer(h, 1, 100, NULL); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintTb(h); return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_TIMER: TbTimer(); return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; int k = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        TrackMouseEvent(&t); g_lastIn = GetTickCount();
        if (k != g_hot) { g_hot = k; InvalidateRect(h, NULL, FALSE); }
        return 0; }
    case WM_MOUSELEAVE: g_hot = H_NONE; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        int k = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (k == H_ORB) { if (IsWindowVisible(g_start)) HideStart(); else if (GetTickCount() - g_startHide > 250) ShowStart(); }
        else {
            HideStart();
            if (k == H_TRAY) TrayMenu(); else if (k == H_CLOCK) OpenSettings(1);
            else if (k == H_DESK) ToggleDesktop(); else if (k >= 0 && k < g_nt) ClickTask(k);
        }
        return 0; }
    case WM_RBUTTONUP: TbRight(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_MBUTTONUP: { int k = Hit(GET_X_LPARAM(l), GET_Y_LPARAM(l)); if (k >= 0 && k < g_nt && g_t[k].pin >= 0) Launch(g_p[g_t[k].pin].path); return 0; }
    case WM_DISPLAYCHANGE: g_scrW = GetSystemMetrics(SM_CXSCREEN); g_scrH = GetSystemMetrics(SM_CYSCREEN); ApplyAll(); return 0;
    case WM_DESTROY:
        KillTimer(h, 1); UnskinAll();
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &g_origWA, SPIF_SENDCHANGE);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
static LRESULT CALLBACK RevProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT c; GetClientRect(h, &c);
        VGrad(dc, &c, RGB(120, 160, 215), RGB(40, 70, 120));
        { RECT g = { c.right / 2 - 14, c.bottom / 2 - 1, c.right / 2 + 14, c.bottom / 2 + 1 }; FillR(dc, &g, RGB(235, 245, 255)); }
        EndPaint(h, &ps); return 0; }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN:
        ShowWindow(g_tb, SW_SHOWNOACTIVATE); g_lastIn = GetTickCount(); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ================= settings window ================= */
enum { ID_H = 100, ID_W, ID_TOP, ID_TITLES, ID_AUTOHIDE, ID_RLEN, ID_RTHK, ID_RPOS,
       ID_CLK, ID_DATE, ID_TFMT, ID_DFMT, ID_TZ, ID_TZOFF,
       ID_NAME, ID_PIC, ID_PICB, ID_FULL, ID_WALL, ID_WALLB, ID_SKIN,
       ID_PLIST, ID_PADD, ID_PDEL, ID_APPLY, ID_CLOSE, ID_TAB };
static HWND g_sc[96]; static int g_st[96], g_nsc;

static HWND C(int tab, const wchar_t *cls, const wchar_t *txt, DWORD st, int x, int y, int w, int h, int id) {
    DWORD ex = (!wcscmp(cls, L"EDIT") || !wcscmp(cls, L"LISTBOX")) ? WS_EX_CLIENTEDGE : 0;
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, g_set, (HMENU)(INT_PTR)id, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_f, TRUE);
    g_sc[g_nsc] = c; g_st[g_nsc++] = tab; return c;
}
static void Row(int tab, const wchar_t *label, int y, int id) {
    C(tab, L"STATIC", label, 0, 20, y + 3, 230, 20, 0);
    C(tab, L"EDIT", L"", ES_AUTOHSCROLL, 260, y, 150, 22, id);
}
static void Chk(int tab, const wchar_t *label, int y, int id) { C(tab, L"BUTTON", label, BS_AUTOCHECKBOX, 20, y, 390, 22, id); }
#define CK(id, v) CheckDlgButton(g_set, id, (v) ? BST_CHECKED : BST_UNCHECKED)

static void FillPins(void) {
    int i; SendDlgItemMessageW(g_set, ID_PLIST, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_np; i++) SendDlgItemMessageW(g_set, ID_PLIST, LB_ADDSTRING, 0, (LPARAM)g_p[i].path);
}
static void FillSettings(void) {
    SetDlgItemInt(g_set, ID_H, g_c.tbH, FALSE); SetDlgItemInt(g_set, ID_W, g_c.tbW, FALSE);
    CK(ID_TOP, g_c.tbTop); CK(ID_TITLES, g_c.titles); CK(ID_AUTOHIDE, g_c.autoHide);
    SetDlgItemInt(g_set, ID_RLEN, g_c.revLen, FALSE); SetDlgItemInt(g_set, ID_RTHK, g_c.revThk, FALSE); SetDlgItemInt(g_set, ID_RPOS, g_c.revPos, FALSE);
    CK(ID_CLK, g_c.showClock); CK(ID_DATE, g_c.showDate);
    SetDlgItemTextW(g_set, ID_TFMT, g_c.timeFmt); SetDlgItemTextW(g_set, ID_DFMT, g_c.dateFmt);
    CK(ID_TZ, g_c.tzFixed); SetDlgItemInt(g_set, ID_TZOFF, (UINT)g_c.tzOff, TRUE);
    SetDlgItemTextW(g_set, ID_NAME, g_c.user); SetDlgItemTextW(g_set, ID_PIC, g_c.pic);
    CK(ID_FULL, g_c.fullDesk); SetDlgItemTextW(g_set, ID_WALL, g_c.wall); CK(ID_SKIN, g_c.skin);
    FillPins();
}
static int GetInt(int id, int def, int lo, int hi) {
    BOOL ok = FALSE; int v = (int)GetDlgItemInt(g_set, id, &ok, TRUE); return ok ? Clamp(v, lo, hi) : def;
}
static void ReadSettings(void) {
    Cfg *c = &g_c;
    c->tbH = GetInt(ID_H, c->tbH, 24, 100); c->tbW = GetInt(ID_W, c->tbW, 20, 100);
    c->tbTop = IsDlgButtonChecked(g_set, ID_TOP) == BST_CHECKED; c->titles = IsDlgButtonChecked(g_set, ID_TITLES) == BST_CHECKED;
    c->autoHide = IsDlgButtonChecked(g_set, ID_AUTOHIDE) == BST_CHECKED;
    c->revLen = GetInt(ID_RLEN, c->revLen, 5, 100); c->revThk = GetInt(ID_RTHK, c->revThk, 2, 40); c->revPos = GetInt(ID_RPOS, c->revPos, 0, 100);
    c->showClock = IsDlgButtonChecked(g_set, ID_CLK) == BST_CHECKED; c->showDate = IsDlgButtonChecked(g_set, ID_DATE) == BST_CHECKED;
    GetDlgItemTextW(g_set, ID_TFMT, c->timeFmt, 64); GetDlgItemTextW(g_set, ID_DFMT, c->dateFmt, 64);
    c->tzFixed = IsDlgButtonChecked(g_set, ID_TZ) == BST_CHECKED; c->tzOff = GetInt(ID_TZOFF, c->tzOff, -720, 840);
    GetDlgItemTextW(g_set, ID_NAME, c->user, 64); GetDlgItemTextW(g_set, ID_PIC, c->pic, MAX_PATH);
    c->fullDesk = IsDlgButtonChecked(g_set, ID_FULL) == BST_CHECKED; GetDlgItemTextW(g_set, ID_WALL, c->wall, MAX_PATH);
    c->skin = IsDlgButtonChecked(g_set, ID_SKIN) == BST_CHECKED;
}
static void ShowTab(int sel) {
    int i; for (i = 0; i < g_nsc; i++) ShowWindow(g_sc[i], (g_st[i] < 0 || g_st[i] == sel) ? SW_SHOW : SW_HIDE);
}
static void BuildSettings(HWND h) {
    HWND tab; TCITEMW ti; static const wchar_t *tn[] = { L"Taskbar", L"Clock", L"Start && Desktop", L"Shortcuts" }; int i;
    tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 8, 8, 430, 330, h, (HMENU)(INT_PTR)ID_TAB, g_hi, NULL);
    SendMessageW(tab, WM_SETFONT, (WPARAM)g_f, TRUE);
    for (i = 0; i < 4; i++) { ZeroMemory(&ti, sizeof ti); ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)tn[i]; SendMessageW(tab, TCM_INSERTITEMW, i, (LPARAM)&ti); }
    Row(0, L"Taskbar height (24-100 px)", 44, ID_H);
    Row(0, L"Taskbar width (20-100 %)", 72, ID_W);
    Chk(0, L"Place taskbar at top of screen", 102, ID_TOP);
    Chk(0, L"Show titles of running apps", 128, ID_TITLES);
    Chk(0, L"Auto-hide taskbar (use reveal bar)", 154, ID_AUTOHIDE);
    Row(0, L"Reveal bar length (% of screen)", 186, ID_RLEN);
    Row(0, L"Reveal bar thickness (px)", 214, ID_RTHK);
    Row(0, L"Reveal bar position (% from left)", 242, ID_RPOS);
    Chk(1, L"Show clock", 46, ID_CLK);
    Chk(1, L"Show date", 72, ID_DATE);
    Row(1, L"Time format", 104, ID_TFMT);
    Row(1, L"Date format", 132, ID_DFMT);
    C(1, L"STATIC", L"Examples:  hh:mm tt   HH:mm:ss   dd-MM-yyyy   ddd, dd MMM", 0, 20, 160, 400, 20, 0);
    Chk(1, L"Use fixed UTC offset instead of Wine timezone", 196, ID_TZ);
    Row(1, L"Offset in minutes (India IST = 330)", 224, ID_TZOFF);
    Row(2, L"Profile name", 44, ID_NAME);
    C(2, L"STATIC", L"Profile picture", 0, 20, 79, 100, 20, 0);
    C(2, L"EDIT", L"", ES_AUTOHSCROLL, 120, 76, 220, 22, ID_PIC); C(2, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 346, 75, 64, 24, ID_PICB);
    Chk(2, L"Full desktop mode (wallpaper + desktop icons)", 112, ID_FULL);
    C(2, L"STATIC", L"Wallpaper", 0, 20, 147, 100, 20, 0);
    C(2, L"EDIT", L"", ES_AUTOHSCROLL, 120, 144, 220, 22, ID_WALL); C(2, L"BUTTON", L"Browse...", BS_PUSHBUTTON, 346, 143, 64, 24, ID_WALLB);
    Chk(2, L"Use Windows 7 style title bars (replace Wine's)", 182, ID_SKIN);
    C(3, L"STATIC", L"Pinned taskbar shortcuts (.exe / .lnk):", 0, 20, 46, 380, 20, 0);
    C(3, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY, 20, 70, 390, 200, ID_PLIST);
    C(3, L"BUTTON", L"Add shortcut...", BS_PUSHBUTTON, 20, 280, 120, 26, ID_PADD);
    C(3, L"BUTTON", L"Remove selected", BS_PUSHBUTTON, 150, 280, 120, 26, ID_PDEL);
    C(-1, L"BUTTON", L"Apply", BS_DEFPUSHBUTTON, 262, 346, 84, 28, ID_APPLY);
    C(-1, L"BUTTON", L"Close", BS_PUSHBUTTON, 354, 346, 84, 28, ID_CLOSE);
    FillSettings(); ShowTab(0);
}
static LRESULT CALLBACK SetProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: g_set = h; BuildSettings(h); return 0;
    case WM_NOTIFY:
        if (((NMHDR *)l)->idFrom == ID_TAB && ((NMHDR *)l)->code == TCN_SELCHANGE) ShowTab((int)SendDlgItemMessageW(h, ID_TAB, TCM_GETCURSEL, 0, 0));
        return 0;
    case WM_COMMAND: {
        wchar_t f[MAX_PATH];
        switch (LOWORD(w)) {
        case ID_APPLY: ReadSettings(); SaveCfg(); ApplyAll(); return 0;
        case ID_CLOSE: ShowWindow(h, SW_HIDE); return 0;
        case ID_PICB: if (PickFile(h, f, IMG_FILTER, L"Choose profile picture")) SetDlgItemTextW(h, ID_PIC, f); return 0;
        case ID_WALLB: if (PickFile(h, f, IMG_FILTER, L"Choose wallpaper")) SetDlgItemTextW(h, ID_WALL, f); return 0;
        case ID_PADD: if (PickFile(h, f, EXE_FILTER, L"Add taskbar shortcut")) { AddPin(f); SavePins(); FillPins(); RefreshTasks(1); } return 0;
        case ID_PDEL: { int i = (int)SendDlgItemMessageW(h, ID_PLIST, LB_GETCURSEL, 0, 0); if (i >= 0) { RemovePin(i); FillPins(); } return 0; }
        }
        return 0; }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
static void OpenSettings(int tab) {
    if (!g_set) {
        CreateWindowExW(0, L"WinShellSettings", L"WinShell Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                        (g_scrW - 460) / 2, Clamp((g_scrH - 420) / 2, 0, g_scrH), 460, 420, NULL, NULL, g_hi, NULL);
    }
    FillSettings();
    SendDlgItemMessageW(g_set, ID_TAB, TCM_SETCURSEL, tab, 0); ShowTab(tab);
    ShowWindow(g_set, SW_SHOW); SetForegroundWindow(g_set);
}

/* ================= apply / main ================= */
static void ApplyAll(void) {
    if (g_pic) DeleteObject(g_pic);
    g_pic = LoadImg(g_c.pic);
    UpdateClock();
    PlaceTaskbar();
    if (g_c.fullDesk) ShowDesk(); else if (g_desk) ShowWindow(g_desk, SW_HIDE);
    if (!g_c.skin) UnskinAll();
    if (g_c.autoHide) g_lastIn = GetTickCount();
    else ShowWindow(g_tb, SW_SHOWNOACTIVATE);
    RefreshTasks(1);
}
static void Reg(const wchar_t *n, WNDPROC p, UINT style, HBRUSH bg) {
    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc; wc.style = style; wc.lpfnWndProc = p; wc.hInstance = g_hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = bg; wc.lpszClassName = n;
    RegisterClassExW(&wc);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    MSG msg; GpInput gi = { 1, NULL, FALSE, FALSE }; INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_TAB_CLASSES };
    (void)hp; (void)show; g_hi = hi;
    if (cmd && wcsstr(cmd, L"/restart")) Sleep(1200);
    CreateMutexW(NULL, TRUE, L"WinShell_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    CoInitialize(NULL); InitCommonControlsEx(&ic); GdiplusStartup(&g_gdip, &gi, NULL);
    GetModuleFileNameW(NULL, g_ini, MAX_PATH); PathRemoveFileSpecW(g_ini); PathAppendW(g_ini, L"winshell.ini");
    g_scrW = GetSystemMetrics(SM_CXSCREEN); g_scrH = GetSystemMetrics(SM_CYSCREEN);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_origWA, 0);
    MkFonts(); LoadCfg();
    Reg(L"WinShellTaskbar", TbProc, CS_DBLCLKS, NULL);
    Reg(L"WinShellStart", StartProc, 0, NULL);
    Reg(L"WinShellReveal", RevProc, 0, NULL);
    Reg(L"WinShellDesktop", DeskProc, CS_DBLCLKS, NULL);
    Reg(L"WinShellDeco", DecoProc, CS_DBLCLKS, NULL);
    Reg(L"WinShellSettings", SetProc, 0, (HBRUSH)(COLOR_BTNFACE + 1));
    g_desk = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WinShellDesktop", L"", WS_POPUP, 0, 0, g_scrW, g_scrH, NULL, NULL, hi, NULL);
    g_start = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"WinShellStart", L"", WS_POPUP, 0, 0, PW, PH, NULL, NULL, hi, NULL);
    g_rev = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED, L"WinShellReveal", L"", WS_POPUP, 0, g_scrH - 6, 100, 6, NULL, NULL, hi, NULL);
    SetLayeredWindowAttributes(g_rev, 0, 200, LWA_ALPHA);
    g_tb = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"WinShellTaskbar", L"WinShell Taskbar", WS_POPUP, 0, 0, 100, g_c.tbH, NULL, NULL, hi, NULL);
    ScanStart(); ApplyAll();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return 0;
}
