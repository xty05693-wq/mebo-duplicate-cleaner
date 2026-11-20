// ═══════════════════════════════════════════════════════════════════
//  Mebo Duplicate Cleaner  ·  v0.5.0  ·  Mebo System
//  Поиск дубликатов файлов. Чистый C++ / WinAPI + GDI+, без зависимостей.
//
//  Сборка (MinGW-w64 / TDM-GCC):
//    g++ -O2 -std=c++17 -municode -mwindows MeboDuplicateCleaner.cpp ^
//      -o MeboDuplicateCleaner.exe ^
//      -ldwmapi -lbcrypt -lole32 -luuid -lshell32 -lshlwapi -lcomdlg32 ^
//      -lgdi32 -luser32 -lgdiplus
//
//  Сборка (MSVC):
//    cl /O2 /EHsc /DUNICODE /D_UNICODE MeboDuplicateCleaner.cpp ^
//       /Fe:MeboDuplicateCleaner.exe user32.lib gdi32.lib dwmapi.lib ^
//       shell32.lib shlwapi.lib ole32.lib bcrypt.lib gdiplus.lib comdlg32.lib
//
//  Запуск: MeboDuplicateCleaner.exe [папка...] — указанная папка сканируется сразу.
//          Несколько папок: аргументами или через вертикальную черту в поле пути: A|B|C
//
//  v0.3.0 — переработан интерфейс: DPI-масштабирование, GDI+ сглаживание,
//  иконки и анимации, карточки групп, быстрый выбор папок, KPI-подвал,
//  контекстное меню, горячие клавиши, запуск папки из командной строки.
//
//  v0.5.0 — фильтры (размер · тип · дата), несколько папок за пуск и пропуск
//  служебных каталогов, правила выбора оригинала (+ «Сделать оригиналом»),
//  экспорт списка в CSV/TXT и в буфер обмена, сортировка групп по выгоде,
//  SHA-256 группы в отчёте, счётчик отсеянных файлов.
// ═══════════════════════════════════════════════════════════════════

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cwctype>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Gdiplus;

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

#define WM_APP_PROGRESS   (WM_APP + 0)   // wParam = фаза, lParam = счётчик
#define WM_APP_DONE       (WM_APP + 1)   // wParam = std::vector<DupGroup>*
#define WM_APP_TOTAL      (WM_APP + 2)   // wParam = кандидатов, lParam = всего файлов
#define WM_APP_AUTOSTART  (WM_APP + 3)   // сканировать папку из командной строки

// ── Версия ─────────────────────────────────────────────────────────
static const wchar_t* APP_VERSION = L"v0.5.0";

// ── Палитра (единая с сайтом mebo-system) ──────────────────────────
static const COLORREF C_BG        = RGB(13, 17, 23);    // фон окна
static const COLORREF C_BG_TOP    = RGB(18, 23, 31);
static const COLORREF C_PANEL     = RGB(22, 27, 34);
static const COLORREF C_PANEL_TOP = RGB(28, 34, 43);
static const COLORREF C_CARD      = RGB(23, 29, 37);
static const COLORREF C_CARD_TOP  = RGB(29, 36, 45);
static const COLORREF C_BORDER    = RGB(48, 54, 61);
static const COLORREF C_BORDER_HI = RGB(72, 82, 96);
static const COLORREF C_FG        = RGB(230, 237, 243);
static const COLORREF C_MUTED     = RGB(139, 148, 158);
static const COLORREF C_FAINT     = RGB(98, 107, 119);
static const COLORREF C_ACCENT    = RGB(79, 140, 255);
static const COLORREF C_ACCENT_HI = RGB(118, 166, 255);
static const COLORREF C_ACCENT_LO = RGB(48, 108, 214);
static const COLORREF C_MINT      = RGB(110, 231, 183);
static const COLORREF C_DANGER    = RGB(218, 54, 51);
static const COLORREF C_DANGER_HI = RGB(236, 92, 84);
static const COLORREF C_AMBER     = RGB(230, 168, 79);
static const COLORREF C_INPUT     = RGB(10, 14, 19);
static const COLORREF C_HOVER     = RGB(38, 45, 56);
static const COLORREF C_DISABLED  = RGB(30, 36, 44);

// ── Иконки (рисуются GDI+ на сетке 24×24) ──────────────────────────
enum IconId
{
    IC_NONE = 0, IC_FOLDER, IC_SEARCH, IC_STOP, IC_X, IC_LIST, IC_CHECKLIST,
    IC_TRASH, IC_OK, IC_WARN, IC_INFO, IC_DOWNLOAD, IC_DOC, IC_MONITOR,
    IC_PICTURE, IC_COPIES, IC_SPINNER, IC_FILTER
};

// ── Кнопки ─────────────────────────────────────────────────────────
enum { B_BROWSE, B_FILTERS, B_SCAN, B_CANCEL, B_UNCHECK, B_AUTO, B_DELETE, B_COUNT };
struct Btn
{
    std::wstring label;
    IconId       icon    = IC_NONE;
    int          style   = 0;      // 0 = привидение, 1 = акцент, 2 = опасность
    RECT         rc      = {};
    bool         enabled = true;
    bool         visible = true;
    float        aHov    = 0.f;    // анимации наведения/нажатия
    float        aPress  = 0.f;
    std::wstring hint;
};

// ── Модель данных ──────────────────────────────────────────────────
struct DupFile { std::wstring path; FILETIME mtime = {}; };
struct DupGroup
{
    long long size = 0;
    std::wstring hash;         // SHA-256 содержимого = идентификатор группы
    std::wstring manualOrig;   // путь, выбранный пользователем как оригинал (пусто = по правилу)
    std::vector<DupFile> files;
};

struct Row
{
    enum Kind { Group, File, Spacer } kind = Spacer;
    std::wstring name, dir, path, text;
    FILETIME mtime      = {};
    long long size      = 0;       // File: размер файла
    long long freeBytes = 0;       // Group: сколько освободится
    int   group = -1;              // индекс группы
    int   indexInGroup = 0;        // File: 0 = оригинал
    bool  checked = false;         // File: помечен к удалению
    float aCheck  = 0.f;           // анимация чекбокса
};

struct Chip { IconId icon = IC_NONE; std::wstring title, path; RECT rc = {}; bool hot = false; float aHov = 0.f; };

// ── Состояние ──────────────────────────────────────────────────────
static HWND      g_hwnd = nullptr, g_edit = nullptr;
static HFONT     g_fLogo, g_fTitle, g_fBold, g_fNorm, g_fSmall, g_fTiny, g_fMono, g_fKpi;
static HBRUSH    g_brInput = nullptr;
static HDC       g_mdc = nullptr;
static Btn       g_btns[B_COUNT];
static Graphics* g_gx = nullptr;   // GDI+ контекст текущего кадра

static std::vector<DupGroup> g_groups;      // то, что видно в списке (после фильтров)
static std::vector<DupGroup> g_allGroups;   // сырой результат сканирования
static std::vector<Row>      g_rows;
static std::vector<Chip>     g_chips;
static std::vector<std::wstring> g_rootsSeen;  // папки последнего сканирования

static bool        g_scanning = false;
static volatile bool g_cancel = false;
static int         g_phase = 0;
static long long   g_progDone = 0, g_progTotal = 0, g_filesSeen = 0;
static ULONGLONG   g_scanStart = 0;
static int         g_marquee = 0;

static double g_scale = 1.0;
static int    g_wndW = 0, g_wndH = 0;

static float  g_scroll = 0.f, g_scrollTarget = 0.f;
static int    g_scrollMax = 0;
static float  g_reveal = 1.f;
static float  g_time = 0.f;
static bool   g_timerOn = false;
static LARGE_INTEGER g_perfFreq, g_perfPrev;

static int   g_hoverBtn = -1, g_pressedBtn = -1, g_hoverRow = -1, g_prevHoverRow = -1;
static float g_rowHov = 0.f;
static bool  g_tracking = false, g_thumbHot = false, g_thumbDrag = false, g_clearHover = false, g_editFocus = false;
static int   g_thumbDragOff = 0;

static int      g_checkedCount = 0;
static long long g_checkedBytes = 0, g_totalFreed = 0, g_maxFree = 1;
static int      g_totalFiles = 0;
static long long g_seenFiles = 0, g_skippedFiles = 0;   // перебрано / отсеяно фильтрами
static std::wstring g_status = L"Выберите папку и нажмите «Сканировать».";
static std::wstring g_hint;
static int g_statusIcon = IC_INFO;

// ── Фильтры и правила (v0.5.0) ──────────────────────────────────
enum { MS_OFF, MS_100K, MS_1M, MS_10M, MS_100M, MS_1G, MS_N };          // минимальный размер
enum { TY_ALL, TY_IMG, TY_DOC, TY_ARCH, TY_MEDIA, TY_CODE, TY_N };      // тип файлов
enum { DF_ANY, DF_7D, DF_30D, DF_365D, DF_OLD, DF_N };                  // дата изменения
enum { OR_OLD, OR_NEW, OR_CLEAN, OR_SHORT, OR_FIRST, OR_N };            // что считать оригиналом

static int  g_ms    = MS_OFF;
static int  g_ty    = TY_ALL;
static int  g_df    = DF_ANY;
static int  g_or    = OR_OLD;
static bool g_skipSvc = true;     // не ходить в скрытые/системные/служебные папки
static std::set<std::wstring> g_exts;    // разрешённые расширения (по g_ty)
static ULONGLONG g_dfMin = 0, g_dfMax = ~0ULL;   // окно дат в 100 нс

static const wchar_t* MS_LABEL[]   = { L"любой размер", L"от 100 КБ", L"от 1 МБ", L"от 10 МБ", L"от 100 МБ", L"от 1 ГБ" };
static const wchar_t* TY_LABEL[]   = { L"все файлы", L"картинки", L"документы", L"архивы", L"музыка и видео", L"код и текст" };
static const wchar_t* DF_LABEL[]   = { L"любая дата", L"за 7 дней", L"за 30 дней", L"за год", L"старше года" };
static const wchar_t* OR_LABEL[]   = { L"самый старый", L"самый новый", L"без «копия (1)» в имени",
                                        L"самый короткий путь", L"первый найденный" };
static const long long MS_BYTES[]  = { 0, 100LL * 1024, 1024LL * 1024, 10LL * 1024 * 1024,
                                       100LL * 1024 * 1024, 1024LL * 1024 * 1024 };
static const wchar_t* TY_EXTS[]    = {
    L"",
    L"jpg jpeg jpe png gif bmp webp tif tiff ico heic heif svg psd raw cr2 nef arw wmf emf jxl avif",
    L"pdf doc docx xls xlsx ppt pptx odt ods odp rtf txt md epub xps dwg vsd",
    L"zip rar 7z tar gz bz2 xz zst iso cab arj lzh",
    L"mp3 wav flac aac ogg wma m4a m4b opus mp4 mkv avi mov wmv m4v webm mpg mpeg 3gp aiff",
    L"js ts tsx jsx cpp c h hpp cs py java go rs php html htm css json xml sql yaml yml rb swift kt lua"
};
static const wchar_t* DIR_BLOCK[]  = { L"$RECYCLE.BIN", L"System Volume Information", L"node_modules",
                                       L".git", L".hg", L".svn", L"__pycache__", L"WinSxS", L"WindowsApps" };
static const wchar_t* FILE_BLOCK[] = { L"desktop.ini", L"Thumbs.db", L"ehthumbs.db", L"ntuser.dat" };

// ── Геометрия ──────────────────────────────────────────────────────
static int PAD, ROW_TOP, H_HEADER, H_TOOLBAR, H_CHIPS, H_STATUS, H_COLHDR, H_FOOTER,
           BTN_H, EDIT_H, ROW_FILE, ROW_GROUP, ROW_SPACER, CB_SZ, BADGE, CARD_X, CARD_PAD;
static RECT g_rHeader, g_rToolbar, g_rChips, g_rStatus, g_rCols, g_rList, g_rFooter,
            g_rEdit, g_rProgress, g_rClear;
static int  g_chipsX0 = 0;

static inline int   S(int v) { return (int)(v * g_scale + 0.5); }
static inline double ClampD(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float  ClampF(float v, float lo, float hi)    { return v < lo ? lo : (v > hi ? hi : v); }
static inline int    MinI(int a, int b) { return a < b ? a : b; }
static inline int    MaxI(int a, int b) { return a > b ? a : b; }
static inline float  MinF(float a, float b) { return a < b ? a : b; }

static void ComputeSizes()
{
    PAD        = S(18);
    ROW_TOP    = S(14);
    H_HEADER   = S(66);
    H_TOOLBAR  = S(64);
    H_CHIPS    = S(46);
    H_STATUS   = S(44);
    H_COLHDR   = S(30);
    H_FOOTER   = S(88);
    BTN_H      = S(36);
    EDIT_H     = S(38);
    ROW_FILE   = S(31);
    ROW_GROUP  = S(38);
    ROW_SPACER = S(11);
    CB_SZ      = S(17);
    BADGE      = S(34);
    CARD_X     = S(12);
    CARD_PAD   = S(7);
}

// ═══════════════════════════════════════════════════════════════════
//  Цвет и GDI+ примитивы
// ═══════════════════════════════════════════════════════════════════
static COLORREF Mix(COLORREF a, COLORREF b, double t)
{
    t = ClampD(t, 0, 1);
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}
static Color ClrA(COLORREF c, double a01)
{
    return Color((BYTE)ClampD(a01 * 255.0, 0, 255), GetRValue(c), GetGValue(c), GetBValue(c));
}
static void GpFlush() { if (g_gx) g_gx->Flush(); }

static void RoundPathF(GraphicsPath& p, float x, float y, float w, float h,
                       float rtl, float rtr, float rbr, float rbl)
{
    rtl = MinF(rtl, w / 2); rtr = MinF(rtr, w / 2); rbr = MinF(rbr, w / 2); rbl = MinF(rbl, w / 2);
    rtl = MinF(rtl, h / 2); rtr = MinF(rtr, h / 2); rbr = MinF(rbr, h / 2); rbl = MinF(rbl, h / 2);
    if (rtl <= 0.01f && rtr <= 0.01f && rbr <= 0.01f && rbl <= 0.01f)
    {
        p.AddRectangle(RectF(x, y, w, h));
        return;
    }
    if (rtl > 0) p.AddArc(x, y, rtl * 2, rtl * 2, 180, 90); else p.StartFigure();
    p.AddLine(x + rtl, y, x + w - rtr, y);
    if (rtr > 0) p.AddArc(x + w - rtr * 2, y, rtr * 2, rtr * 2, 270, 90);
    p.AddLine(x + w, y + rtr, x + w, y + h - rbr);
    if (rbr > 0) p.AddArc(x + w - rbr * 2, y + h - rbr * 2, rbr * 2, rbr * 2, 0, 90);
    p.AddLine(x + w - rbr, y + h, x + rbl, y + h);
    if (rbl > 0) p.AddArc(x, y + h - rbl * 2, rbl * 2, rbl * 2, 90, 90);
    p.AddLine(x, y + h - rbl, x, y + rtl);
    p.CloseFigure();
}
static void RoundPathF(GraphicsPath& p, const RECT& r, float rad)
{
    RoundPathF(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top),
               rad, rad, rad, rad);
}
static bool ValidRc(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

static void FillRound(const RECT& r, float rad, COLORREF top, COLORREF bot, double alpha = 1.0)
{
    if (!g_gx || !ValidRc(r) || alpha <= 0) return;
    GraphicsPath p; RoundPathF(p, r, rad);
    RectF bb((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
    LinearGradientBrush br(bb, ClrA(top, alpha), ClrA(bot, alpha), 90.f, false);
    g_gx->FillPath(&br, &p);
}
static void FillSolid(const RECT& r, float rad, COLORREF c, double alpha = 1.0)
{
    if (!g_gx || !ValidRc(r) || alpha <= 0) return;
    GraphicsPath p; RoundPathF(p, r, rad);
    SolidBrush br(ClrA(c, alpha));
    g_gx->FillPath(&br, &p);
}
static void StrokeRound(const RECT& r, float rad, COLORREF c, double alpha = 1.0, float w = 1.f)
{
    if (!g_gx || !ValidRc(r) || alpha <= 0) return;
    GraphicsPath p; RoundPathF(p, r, rad);
    Pen pen(ClrA(c, alpha), w);
    pen.SetAlignment(PenAlignmentInset);
    g_gx->DrawPath(&pen, &p);
}
static void Glow(const RECT& r, float rad, COLORREF c, double alpha)
{
    if (!g_gx || !ValidRc(r) || alpha <= 0) return;
    GraphicsPath p; RoundPathF(p, r, rad);
    PathGradientBrush br(&p);
    br.SetCenterColor(ClrA(c, alpha));
    INT cnt = 1; Color sur = ClrA(c, 0);
    br.SetSurroundColors(&sur, &cnt);
    g_gx->FillPath(&br, &p);
}
static void LineA(float x0, float y0, float x1, float y1, COLORREF c, double alpha, float w)
{
    if (!g_gx || alpha <= 0) return;
    Pen pen(ClrA(c, alpha), w);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    g_gx->DrawLine(&pen, x0, y0, x1, y1);
}
static void PolyA(const PointF* pts, int n, COLORREF c, double alpha, float w)
{
    if (!g_gx || alpha <= 0 || n < 2) return;
    Pen pen(ClrA(c, alpha), w);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    g_gx->DrawLines(&pen, pts, n);
}
static void DotA(float cx, float cy, float rad, COLORREF c, double alpha)
{
    if (!g_gx || alpha <= 0 || rad <= 0) return;
    SolidBrush br(ClrA(c, alpha));
    g_gx->FillEllipse(&br, cx - rad, cy - rad, rad * 2, rad * 2);
}
static void RingA(float cx, float cy, float rad, COLORREF c, double alpha, float w)
{
    if (!g_gx || alpha <= 0) return;
    Pen pen(ClrA(c, alpha), w);
    g_gx->DrawEllipse(&pen, cx - rad, cy - rad, rad * 2, rad * 2);
}

// ═══════════════════════════════════════════════════════════════════
//  Текст
// ═══════════════════════════════════════════════════════════════════
static int MeasW(const std::wstring& s, HFONT f)
{
    if (!g_mdc) g_mdc = CreateCompatibleDC(nullptr);
    if (!g_mdc || s.empty() || !f) return 0;
    HFONT old = (HFONT)SelectObject(g_mdc, f);
    SIZE sz = {}; GetTextExtentPoint32W(g_mdc, s.c_str(), (int)s.size(), &sz);
    SelectObject(g_mdc, old);
    return sz.cx;
}
static RECT VR(int x0, int x1, int yc, int half) { RECT r = { x0, yc - half, x1, yc + half }; return r; }

static void Txt(HDC dc, const std::wstring& s, RECT rc, COLORREF c, HFONT f,
                UINT flags = DT_LEFT, double alpha = 1.0)
{
    if (s.empty() || !ValidRc(rc)) return;
    GpFlush();
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, alpha >= 1.0 ? c : Mix(c, C_CARD, 1.0 - alpha));
    HFONT old = (HFONT)SelectObject(dc, f);
    UINT fl = flags | DT_SINGLELINE | DT_NOPREFIX;
    if (!(fl & (DT_PATH_ELLIPSIS | DT_END_ELLIPSIS | DT_WORD_ELLIPSIS))) fl |= DT_END_ELLIPSIS;
    if (!(fl & (DT_TOP | DT_BOTTOM | DT_CALCRECT))) fl |= DT_VCENTER;
    DrawTextW(dc, s.c_str(), (int)s.size(), &rc, fl);
    SelectObject(dc, old);
}
// подпись UpCase с межбуквенным интервалом
static int TrackedWidth(const std::wstring& s, HFONT f, int track)
{
    int total = 0;
    for (size_t i = 0; i < s.size(); ++i) total += MeasW(s.substr(i, 1), f) + (i ? track : 0);
    return total;
}
static void TxtTracked(HDC dc, const std::wstring& s, int x, int yc, COLORREF c, HFONT f,
                       int track, bool rightAlign = false, double alpha = 1.0)
{
    int total = TrackedWidth(s, f, track);
    int px = rightAlign ? x - total : x;
    GpFlush();
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, alpha >= 1.0 ? c : Mix(c, C_CARD, 1.0 - alpha));
    HFONT old = (HFONT)SelectObject(dc, f);
    SIZE sz = {};
    for (size_t i = 0; i < s.size(); ++i)
    {
        std::wstring ch = s.substr(i, 1);
        GetTextExtentPoint32W(dc, ch.c_str(), 1, &sz);
        TextOutW(dc, px, yc - sz.cy / 2, ch.c_str(), 1);
        px += sz.cx + track;
    }
    SelectObject(dc, old);
}

// ═══════════════════════════════════════════════════════════════════
//  Иконки
// ═══════════════════════════════════════════════════════════════════
static void DrawIcon(IconId ic, int cx, int cy, int size, COLORREF color,
                     double alpha = 1.0, float phase = 0.f)
{
    if (ic == IC_NONE || alpha <= 0 || !g_gx || size <= 0) return;
    double k  = size / 24.0;
    double ox = cx - size / 2.0, oy = cy - size / 2.0;
    float  w  = (float)MaxI(1, (int)(1.9 * k));
    struct G
    {
        double ox, oy, k;
        float X(double u) const { return (float)(ox + u * k); }
        float Y(double u) const { return (float)(oy + u * k); }
        RECT R(double a, double b, double c, double d) const
        { RECT r = { (int)X(a), (int)Y(b), (int)X(c), (int)Y(d) }; return r; }
    } G_ = { ox, oy, k };
    auto X = [&](double u) { return G_.X(u); };
    auto Y = [&](double u) { return G_.Y(u); };
    (void)Y;

    switch (ic)
    {
        case IC_FOLDER:
        {
            RECT b = G_.R(2, 8, 22, 20);
            StrokeRound(b, (float)(2 * k), color, alpha, w);
            PointF t[] = { { X(2), Y(8) }, { X(2), Y(5.4) }, { X(8), Y(5.4) }, { X(10.2), Y(8) } };
            PolyA(t, 4, color, alpha, w);
            break;
        }
        case IC_SEARCH:
        {
            RingA(X(10.5), Y(10.5), (float)(6 * k), color, alpha, w);
            LineA(X(14.8), Y(14.8), X(20), Y(20), color, alpha, w);
            break;
        }
        case IC_STOP:
        {
            RECT b = G_.R(6, 6, 18, 18);
            FillSolid(b, (float)(3 * k), color, alpha);
            break;
        }
        case IC_X:
            LineA(X(6), Y(6), X(18), Y(18), color, alpha, w);
            LineA(X(18), Y(6), X(6), Y(18), color, alpha, w);
            break;
        case IC_LIST:
            LineA(X(4), Y(7),  X(15), Y(7),  color, alpha, w);
            LineA(X(4), Y(12), X(19), Y(12), color, alpha, w);
            LineA(X(4), Y(17), X(13), Y(17), color, alpha, w);
            break;
        case IC_CHECKLIST:
        {
            PointF ch[] = { { X(2), Y(12) }, { X(5.2), Y(15.2) }, { X(11), Y(8.4) } };
            PolyA(ch, 3, color, alpha, w);
            LineA(X(13), Y(9),  X(21), Y(9),  color, alpha, w);
            LineA(X(13), Y(16), X(21), Y(16), color, alpha, w);
            break;
        }
        case IC_TRASH:
        {
            LineA(X(3), Y(6.5), X(21), Y(6.5), color, alpha, w);
            PointF h[] = { { X(8.5), Y(6.5) }, { X(8.5), Y(4) }, { X(15.5), Y(4) }, { X(15.5), Y(6.5) } };
            PolyA(h, 4, color, alpha, w);
            PointF bd[] = { { X(5), Y(6.8) }, { X(19), Y(6.8) }, { X(17.6), Y(20.5) }, { X(6.4), Y(20.5) } };
            PolyA(bd, 4, color, alpha, w);
            LineA(X(10), Y(10.5), X(9.7), Y(17), color, alpha, w);
            LineA(X(14), Y(10.5), X(14.3), Y(17), color, alpha, w);
            break;
        }
        case IC_OK:
        {
            RingA(X(12), Y(12), (float)(9 * k), color, alpha, w);
            PointF ch[] = { { X(7.5), Y(12.2) }, { X(10.8), Y(15.5) }, { X(16.6), Y(9) } };
            PolyA(ch, 3, color, alpha, w);
            break;
        }
        case IC_WARN:
        {
            PointF tri[] = { { X(12), Y(3.5) }, { X(21.6), Y(20) }, { X(2.4), Y(20) } };
            Pen pen(ClrA(color, alpha), w);
            pen.SetLineJoin(LineJoinRound); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
            g_gx->DrawPolygon(&pen, tri, 3);
            LineA(X(12), Y(9.5), X(12), Y(14.5), color, alpha, w);
            DotA(X(12), Y(17.6), (float)(1.15 * k), color, alpha);
            break;
        }
        case IC_INFO:
        {
            RingA(X(12), Y(12), (float)(9 * k), color, alpha, w);
            LineA(X(12), Y(11), X(12), Y(17), color, alpha, w);
            DotA(X(12), Y(7.8), (float)(1.2 * k), color, alpha);
            break;
        }
        case IC_DOWNLOAD:
        {
            LineA(X(12), Y(3.5), X(12), Y(14.5), color, alpha, w);
            PointF ar[] = { { X(7), Y(10) }, { X(12), Y(15) }, { X(17), Y(10) } };
            PolyA(ar, 3, color, alpha, w);
            PointF tr[] = { { X(3.5), Y(16) }, { X(3.5), Y(20.5) }, { X(20.5), Y(20.5) }, { X(20.5), Y(16) } };
            PolyA(tr, 4, color, alpha, w);
            break;
        }
        case IC_DOC:
        {
            RECT b = G_.R(5.5, 3, 18.5, 21);
            StrokeRound(b, (float)(3 * k), color, alpha, w);
            LineA(X(9), Y(9),    X(15), Y(9),    color, alpha, w * 0.85f);
            LineA(X(9), Y(12.5), X(15), Y(12.5), color, alpha, w * 0.85f);
            LineA(X(9), Y(16),   X(13), Y(16),   color, alpha, w * 0.85f);
            break;
        }
        case IC_MONITOR:
        {
            RECT b = G_.R(2.5, 4.5, 21.5, 16);
            StrokeRound(b, (float)(2.5 * k), color, alpha, w);
            LineA(X(12), Y(16), X(12), Y(19.5), color, alpha, w);
            LineA(X(8), Y(20),  X(16), Y(20),   color, alpha, w);
            break;
        }
        case IC_PICTURE:
        {
            RECT b = G_.R(2.5, 5, 21.5, 19);
            StrokeRound(b, (float)(3 * k), color, alpha, w);
            DotA(X(8), Y(10), (float)(1.6 * k), color, alpha * 0.9);
            PointF m1[] = { { X(5), Y(16.5) }, { X(10), Y(11.5) }, { X(14), Y(15.5) } };
            PolyA(m1, 3, color, alpha, w * 0.9f);
            PointF m2[] = { { X(12.5), Y(13.5) }, { X(16), Y(10.5) }, { X(19.5), Y(14.5) } };
            PolyA(m2, 3, color, alpha, w * 0.9f);
            break;
        }
        case IC_COPIES:
        {
            RECT b1 = G_.R(8, 3, 21, 16);
            StrokeRound(b1, (float)(3 * k), color, alpha * 0.5, w);
            RECT b2 = G_.R(3, 8, 16, 21);
            FillSolid(b2, (float)(3 * k), color, alpha * 0.16);
            StrokeRound(b2, (float)(3 * k), color, alpha, w);
            break;
        }
        case IC_FILTER:
        {
            PointF fn[] = { { X(3), Y(4.5) }, { X(21), Y(4.5) }, { X(13.6), Y(12.6) }, { X(13.6), Y(20) },
                            { X(10.4), Y(20) }, { X(10.4), Y(12.6) }, { X(3), Y(4.5) } };
            PolyA(fn, 7, color, alpha, w);
            DotA(X(17.5), Y(7.2), (float)(1.1 * k), color, alpha * 0.9);
            break;
        }
        case IC_SPINNER:
        {
            float d = (float)(18 * k);
            float x = X(3), y = Y(3);
            for (int i = 0; i < 3; ++i)
            {
                Pen pen(ClrA(color, alpha * (1.0 - i * 0.33)), w);
                pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
                g_gx->DrawArc(&pen, x, y, d, d, phase * 57.29578f + i * 110.f, 68.f - i * 14.f);
            }
            break;
        }
        default: break;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Утилиты данных
// ═══════════════════════════════════════════════════════════════════
static std::wstring BaseName(const std::wstring& p)
{
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}
static std::wstring DirName(const std::wstring& p)
{
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"" : p.substr(0, k);
}
static std::wstring FmtBytes(long long b)
{
    static const wchar_t* u[] = { L"Б", L"КБ", L"МБ", L"ГБ", L"ТБ" };
    int i = 0; double v = (double)b;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[64];
    if (i == 0) wsprintfW(buf, L"%d Б", (int)b);
    else {
        int whole = (int)v, frac = (int)((v - whole) * 10.0);
        wsprintfW(buf, L"%d.%d %s", whole, frac, u[i]);
    }
    return buf;
}
static std::wstring PluralRu(long long n, const wchar_t* one, const wchar_t* few, const wchar_t* many)
{
    long long d = n % 10, h = n % 100;
    if (d == 1 && h != 11) return one;
    if (d >= 2 && d <= 4 && (h < 10 || h >= 20)) return few;
    return many;
}
// «1 234» с неразрывными пробелами
static std::wstring FmtNum(long long v)
{
    std::wstring s = std::to_wstring(v), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it)
    {
        out.push_back(*it);
        if (++c % 3 == 0 && it + 1 != s.rend()) out.push_back(L'\xA0');
    }
    return std::wstring(out.rbegin(), out.rend());
}
static std::wstring FmtTime(double sec)
{
    if (sec < 0 || sec > 86400) return L"—";
    wchar_t b[64];
    if (sec < 60) wsprintfW(b, L"%.0f с", sec + 0.5);
    else wsprintfW(b, L"%d мин %02d с", (int)sec / 60, (int)sec % 60);
    return b;
}

// ── SHA-256 (BCrypt) ───────────────────────────────────────────────
static bool Sha256File(const std::wstring& path, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, L"SHA256", nullptr, 0))) return false;
    bool ok = false;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0)))
    {
        HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hf != INVALID_HANDLE_VALUE)
        {
            static char buf[1 << 16];
            DWORD rd = 0; ok = true;
            while (ReadFile(hf, buf, (DWORD)sizeof(buf), &rd, nullptr) && rd > 0)
                if (!NT_SUCCESS(BCryptHashData(hHash, (PUCHAR)buf, rd, 0))) { ok = false; break; }
            CloseHandle(hf);
            if (ok) ok = NT_SUCCESS(BCryptFinishHash(hHash, out, 32, 0));
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}
static std::wstring ToHex(const unsigned char* d)
{
    static const wchar_t* H = L"0123456789abcdef";
    std::wstring s; s.reserve(64);
    for (int i = 0; i < 32; ++i) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
    return s;
}

// ═══════════════════════════════════════════════════════════════
//  Фильтры, правила оригинала, разбор списка папок  (v0.5.0)
// ═══════════════════════════════════════════════════════════════
static std::wstring ExtOfName(const std::wstring& name)
{
    size_t slash = name.find_last_of(L"\\/");
    size_t d = name.find_last_of(L'.');
    if (d == std::wstring::npos) return L"";
    if (slash != std::wstring::npos && d < slash) return L"";   // точка в имени папки
    if (d + 1 >= name.size()) return L"";
    std::wstring e = name.substr(d + 1);
    if (e.size() > 12) return L"";
    for (auto& ch : e) ch = (wchar_t)towlower(ch);
    return e;
}
static void RefreshExtSet()
{
    g_exts.clear();
    if (g_ty == TY_ALL) return;
    std::wstring s = TY_EXTS[g_ty];
    size_t p = 0;
    while (p <= s.size())
    {
        size_t q = s.find(L' ', p);
        std::wstring w = s.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p);
        if (!w.empty()) g_exts.insert(w);
        if (q == std::wstring::npos) break;
        p = q + 1;
    }
}
static void RefreshDateWindow()
{
    const ULONGLONG DAY = 864000000000ULL;              // сутки в 100 нс
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULONGLONG t = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    g_dfMin = 0; g_dfMax = ~0ULL;
    switch (g_df)
    {
        case DF_7D:   g_dfMin = t - 7ULL * DAY;   g_dfMax = t; break;
        case DF_30D:  g_dfMin = t - 30ULL * DAY;  g_dfMax = t; break;
        case DF_365D: g_dfMin = t - 365ULL * DAY; g_dfMax = t; break;
        case DF_OLD:  g_dfMax = t - 365ULL * DAY; break;
        default: break;
    }
}
static inline ULONGLONG F2U(const FILETIME& f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}
static inline FILETIME U2F(ULONGLONG u)
{
    FILETIME f = {}; f.dwLowDateTime = (DWORD)(u & 0xFFFFFFFF); f.dwHighDateTime = (DWORD)(u >> 32); return f;
}
// 0 = имя похоже на оригинал; чем больше — тем сильнее «копия»
static int CopyScore(const std::wstring& name)
{
    std::wstring low; low.reserve(name.size());
    for (wchar_t ch : name) low.push_back((wchar_t)towlower(ch));
    int s = 0;
    static const wchar_t* marks[] = { L"копия", L"copy", L"backup", L"резерв", L"старый", L"старая", L"старое" };
    for (auto m : marks) if (low.find(m) != std::wstring::npos) ++s;
    // «(1)», « (2)», «-копия 2», «_2» перед расширением
    size_t dot = low.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? low : low.substr(0, dot);
    for (size_t i = 0; i + 1 < stem.size(); ++i)
        if (stem[i] == L'(' && iswdigit(stem[i + 1])) { ++s; break; }
    if (!stem.empty() && iswdigit(stem[stem.size() - 1])) ++s;
    size_t dash = stem.find_last_of(L"-_");
    if (dash != std::wstring::npos && dash + 2 <= stem.size() && iswdigit(stem[dash + 1])) ++s;
    return s;
}
static bool PassSize(long long size)      { return size >= MS_BYTES[g_ms]; }
static bool PassType(const std::wstring& name)
{
    if (g_ty == TY_ALL) return true;
    return g_exts.count(ExtOfName(name)) != 0;
}
static bool PassDate(const FILETIME& m)   { return F2U(m) >= g_dfMin && F2U(m) <= g_dfMax; }
static bool PassAll(long long size, const std::wstring& name, const FILETIME& m)
{
    return PassSize(size) && PassType(name) && PassDate(m);
}
static bool DirBlocked(const std::wstring& name, DWORD attr)
{
    if (!g_skipSvc) return false;
    if (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return true;
    for (auto b : DIR_BLOCK) if (!_wcsicmp(name.c_str(), b)) return true;
    return false;
}
static bool FileBlocked(const std::wstring& name, DWORD attr)
{
    if (!g_skipSvc) return false;
    if (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return true;
    for (auto b : FILE_BLOCK) if (!_wcsicmp(name.c_str(), b)) return true;
    return false;
}
static std::wstring TrimWs(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\"'"), b = s.find_last_not_of(L" \t\"'");
    if (a == std::wstring::npos) return L"";
    return s.substr(a, b - a + 1);
}
// «A|B|C» → список существующих папок: дубликаты и вложенные одна в другую убираются
static std::vector<std::wstring> ParseRoots(const std::wstring& raw)
{
    std::vector<std::wstring> out;
    std::set<std::wstring> seen;
    size_t p = 0;
    while (p <= raw.size())
    {
        size_t q = raw.find(L'|', p);
        std::wstring part = TrimWs(raw.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p));
        if (!part.empty())
        {
            while (part.size() > 3 && (part.back() == L'\\' || part.back() == L'/')) part.pop_back();
            DWORD attr = GetFileAttributesW(part.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::wstring key = part;
                for (auto& ch : key) ch = (wchar_t)towlower(ch);
                if (key.size() > 1 && key.back() != L'\\') key += L'\\';
                if (seen.insert(key).second) out.push_back(part);
            }
        }
        if (q == std::wstring::npos) break;
        p = q + 1;
    }
    // короткая сортировка по длине: родитель идёт раньше потомка
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() < b.size(); });
    std::vector<std::wstring> top;
    for (auto& r : out)
    {
        bool inside = false;
        std::wstring low = r;
        for (auto& ch : low) ch = (wchar_t)towlower(ch);
        for (auto& t : top)
        {
            std::wstring tl = t;
            for (auto& ch : tl) ch = (wchar_t)towlower(ch);
            if (low.size() > tl.size() && low.compare(0, tl.size(), tl) == 0 &&
                (tl.back() == L'\\' || low[tl.size()] == L'\\' || low[tl.size()] == L'/'))
                { inside = true; break; }
        }
        if (!inside) top.push_back(r);
    }
    return top;
}
static bool FiltersActive()
{
    return g_ms != MS_OFF || g_ty != TY_ALL || g_df != DF_ANY || !g_skipSvc;
}
static int ManualOrigCount()
{
    int n = 0;
    for (auto& g : g_allGroups) if (!g.manualOrig.empty()) ++n;
    return n;
}
static std::wstring FiltersSummary()
{
    std::wstring s;
    auto add = [&](const std::wstring& p) { if (!s.empty()) s += L" · "; s += p; };
    if (g_ms   != MS_OFF) add(MS_LABEL[g_ms]);
    if (g_ty   != TY_ALL) add(TY_LABEL[g_ty]);
    if (g_df   != DF_ANY) add(DF_LABEL[g_df]);
    if (!g_skipSvc)       add(L"включая служебные папки");
    return s;
}
static std::wstring CurrentRootLabel()
{
    if (g_rootsSeen.empty()) return L"";
    if (g_rootsSeen.size() == 1)
    {
        std::wstring n = BaseName(g_rootsSeen[0]);
        return n.empty() ? g_rootsSeen[0] : n;
    }
    return std::to_wstring(g_rootsSeen.size()) + L" " +
           PluralRu((long long)g_rootsSeen.size(), L"папка", L"папки", L"папок");
}
// индекс файла, который считается оригиналом по выбранному правилу
static int PickOriginal(const DupGroup& g)
{
    if (g.files.size() < 2) return 0;
    int best = 0;
    for (size_t i = 1; i < g.files.size(); ++i)
    {
        const DupFile& a = g.files[i];
        const DupFile& b = g.files[best];
        std::wstring na = BaseName(a.path), nb = BaseName(b.path);
        bool better = false;
        switch (g_or)
        {
            case OR_NEW:   better = F2U(a.mtime) >  F2U(b.mtime); break;
            case OR_CLEAN: better = CopyScore(na) < CopyScore(nb) ||
                                    (CopyScore(na) == CopyScore(nb) && a.path.size() < b.path.size()); break;
            case OR_SHORT: better = a.path.size() < b.path.size() ||
                                    (a.path.size() == b.path.size() && _wcsicmp(a.path.c_str(), b.path.c_str()) < 0); break;
            case OR_FIRST: better = false; break;
            case OR_OLD:
            default:       better = F2U(a.mtime) <  F2U(b.mtime) ||
                                    (F2U(a.mtime) == F2U(b.mtime) && a.path.size() < b.path.size()); break;
        }
        if (better) best = (int)i;
    }
    return best;
}
// ставит выбранный оригинал на нулевую позицию (остальные сохраняют порядок)
static void ApplyOrigRule(DupGroup& g)
{
    int k = PickOriginal(g);
    if (k > 0)
    {
        DupFile orig = g.files[k];
        g.files.erase(g.files.begin() + k);
        g.files.insert(g.files.begin(), orig);
    }
}
static std::wstring FmtFileTime(const FILETIME& f)
{
    SYSTEMTIME utc, st;
    if (!FileTimeToSystemTime(&f, &utc)) return L"";
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st)) st = utc;
    wchar_t b[32];
    wsprintfW(b, L"%02d.%02d.%04d %02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    return b;
}
static std::wstring StampNow()
{
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[32];
    wsprintfW(b, L"%04d%02d%02d_%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

// ═══════════════════════════════════════════════════════════════
//  Экспорт списка (CSV / TXT / буфер обмена)
// ═══════════════════════════════════════════════════════════════
static std::wstring CsvCell(const std::wstring& v, wchar_t sep)
{
    bool need = v.find(sep) != std::wstring::npos || v.find(L'"') != std::wstring::npos ||
                v.find(L'\n') != std::wstring::npos;
    if (!need) return v;
    std::wstring o = L"\"";
    for (wchar_t ch : v) { if (ch == L'"') o += L"\"\""; else o += ch; }
    o += L"\"";
    return o;
}
// csv=true — таблицы с разделителем «;» и BOM; false — читаемый отчёт
static std::wstring BuildExport(bool csv)
{
    const wchar_t sep = L';';
    std::wstring out;
    long long totalFree = 0, dupCnt = 0;
    for (auto& g : g_groups)
    {
        totalFree += g.size * ((long long)g.files.size() - 1);
        dupCnt += (long long)g.files.size() - 1;
    }
    if (csv)
    {
        const wchar_t* hdr[] = { L"Группа", L"Размер файла", L"Файлов в группе", L"Освободится",
                                 L"SHA-256", L"Роль", L"К удалению", L"Изменён", L"Имя", L"Папка", L"Полный путь" };
        for (int i = 0; i < 11; ++i) { if (i) out += sep; out += hdr[i]; }
        out += L"\r\n";
        int gi = 0;
        for (auto& g : g_groups)
        {
            ++gi;
            long long freeB = g.size * ((long long)g.files.size() - 1);
            std::wstring freeS = FmtBytes(freeB);
            for (size_t i = 0; i < g.files.size(); ++i)
            {
                bool isOrig = i == 0;
                std::wstring row;
                std::wstring cells[] = {
                    std::to_wstring(gi), FmtBytes(g.size), std::to_wstring(g.files.size()), freeS,
                    g.hash, isOrig ? L"оригинал" : L"дубликат", isOrig ? L"нет" : L"да",
                    FmtFileTime(g.files[i].mtime), BaseName(g.files[i].path), DirName(g.files[i].path), g.files[i].path
                };
                for (int c = 0; c < 11; ++c) { if (c) row += sep; row += CsvCell(cells[c], sep); }
                out += row + L"\r\n";
            }
        }
        return out;
    }
    out = L"Mebo Duplicate Cleaner " + std::wstring(APP_VERSION) + L" — отчёт о дубликатах\r\n";
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    out += L"Дата: " + FmtFileTime(nowFt) + L"\r\n";
    out += L"Папки: ";
    for (size_t i = 0; i < g_rootsSeen.size(); ++i) { if (i) out += L"\r\n       "; out += g_rootsSeen[i]; }
    if (g_rootsSeen.empty()) out += L"—";
    out += L"\r\n";
    std::wstring fs = FiltersSummary();
    out += L"Фильтры: " + (fs.empty() ? std::wstring(L"нет") : fs) + L"\r\n";
    out += L"Оригинал: " + std::wstring(OR_LABEL[g_or]) + L"\r\n";
    out += L"Групп: " + FmtNum((long long)g_groups.size()) + L" · копий: " + FmtNum(dupCnt) +
           L" · освободится: " + FmtBytes(totalFree) + L"\r\n";
    out += L"════════════════════════════════════════════════════════════\r\n\r\n";
    int gi = 0;
    for (auto& g : g_groups)
    {
        ++gi;
        long long freeB = g.size * ((long long)g.files.size() - 1);
        out += L"Группа " + std::to_wstring(gi) + L" — " + std::to_wstring(g.files.size()) + L" " +
               PluralRu((long long)g.files.size(), L"файл", L"файла", L"файлов") +
               L" по " + FmtBytes(g.size) + L" (освободится " + FmtBytes(freeB) + L")\r\n";
        out += L"  SHA-256: " + g.hash + L"\r\n";
        for (size_t i = 0; i < g.files.size(); ++i)
            out += std::wstring(i == 0 ? L"  [оригинал]  " : L"  [дубликат]  ") + g.files[i].path + L"\r\n";
        out += L"\r\n";
    }
    if (g_groups.empty()) out += L"Дубликатов не найдено.\r\n";
    out += L"\r\nСформировано программой Mebo Duplicate Cleaner — ничего не удалялось автоматически.\r\n";
    return out;
}

// ═══════════════════════════════════════════════════════════════════
//  Рабочий поток сканирования
// ═══════════════════════════════════════════════════════════════════
struct Ent { std::wstring path; long long size; FILETIME mtime; };

// итеративный обход в глубину: фильтры применяются на ходу, в память
// попадают только файлы, прошедшие фильтр по размеру и типу
static void CollectFiles(const std::wstring& root, std::vector<Ent>& out,
                         HWND hwnd, long long& seen, long long& skipped)
{
    std::vector<std::wstring> stack; stack.push_back(root);
    while (!stack.empty() && !g_cancel)
    {
        std::wstring dir = stack.back(); stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // симлинки и junction'ы
            std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!DirBlocked(fd.cFileName, fd.dwFileAttributes)) stack.push_back(full);
                else ++skipped;
            }
            else
            {
                long long sz = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                ++seen;
                if (sz <= 0 || FileBlocked(fd.cFileName, fd.dwFileAttributes) ||
                    !PassAll(sz, fd.cFileName, fd.ftLastWriteTime)) { ++skipped; continue; }
                out.push_back({ full, sz, fd.ftLastWriteTime });
                if (seen % 1500 == 0) PostMessageW(hwnd, WM_APP_PROGRESS, 0, (LPARAM)(int)seen);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

struct ScanCtx { HWND hwnd; std::vector<std::wstring> roots; };

// из перебранного списка файлов — готовые группы дубликатов (тот же код использует и самопроверка)
static std::vector<DupGroup>* MakeGroups(std::vector<Ent>& pool, HWND hw, long long& candTotal, long long& allFiles)
{
    // ── группировка по размеру: сортировка вместо map, меньше аллокаций ──
    std::sort(pool.begin(), pool.end(), [](const Ent& a, const Ent& b) {
        if (a.size != b.size) return a.size < b.size;
        return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
    });
    std::vector<const Ent*> cands;
    allFiles = (long long)pool.size();
    for (size_t i = 0; i < pool.size(); )
    {
        size_t j = i + 1;
        while (j < pool.size() && pool[j].size == pool[i].size) ++j;
        if (j - i > 1) for (size_t k = i; k < j; ++k) cands.push_back(&pool[k]);
        i = j;
    }
    candTotal = (long long)cands.size();
    if (hw) PostMessageW(hw, WM_APP_TOTAL, (WPARAM)candTotal, (LPARAM)allFiles);

    // ── SHA-256 кандидатов, затем группировка по хэшу ──
    std::vector<std::pair<std::wstring, const Ent*>> hashed;
    hashed.reserve((size_t)candTotal);
    long long done = 0;
    for (auto* e : cands)
    {
        if (g_cancel) break;
        unsigned char d[32];
        if (Sha256File(e->path, d)) hashed.push_back({ ToHex(d), e });
        if (++done % 8 == 0 || done == candTotal)
            if (hw) PostMessageW(hw, WM_APP_PROGRESS, 1, (LPARAM)(int)done);
    }
    std::sort(hashed.begin(), hashed.end(),
              [](const std::pair<std::wstring, const Ent*>& a,
                 const std::pair<std::wstring, const Ent*>& b) {
        if (a.first != b.first) return a.first < b.first;
        return _wcsicmp(a.second->path.c_str(), b.second->path.c_str()) < 0;
    });

    std::vector<DupGroup>* out = new std::vector<DupGroup>();
    if (!g_cancel)
    {
        for (size_t i = 0; i < hashed.size(); )
        {
            size_t j = i + 1;
            while (j < hashed.size() && hashed[j].first == hashed[i].first) ++j;
            if (j - i > 1)
            {
                DupGroup g;
                g.size = hashed[i].second->size;
                g.hash = hashed[i].first;
                for (size_t k = i; k < j; ++k)          // одна и та же папка в двух корнях не удваивает файл
                {
                    if (!g.files.empty() && _wcsicmp(g.files.back().path.c_str(),
                                                     hashed[k].second->path.c_str()) == 0) continue;
                    g.files.push_back({ hashed[k].second->path, hashed[k].second->mtime });
                }
                if (g.files.size() > 1) out->push_back(g);
            }
            i = j;
        }
        // крупные группы наверх: список читается как план «что мне это даст»
        std::sort(out->begin(), out->end(), [](const DupGroup& a, const DupGroup& b) {
            long long fa = a.size * ((long long)a.files.size() - 1);
            long long fb = b.size * ((long long)b.files.size() - 1);
            if (fa != fb) return fa > fb;
            return a.hash < b.hash;
        });
    }
    hashed.clear();
    return out;
}

static DWORD WINAPI ScanThread(LPVOID p)
{
    ScanCtx* ctx = (ScanCtx*)p;
    HWND hw = ctx->hwnd;

    std::vector<Ent> pool;
    long long seen = 0, skipped = 0;
    for (auto& r : ctx->roots)
    {
        if (g_cancel) break;
        CollectFiles(r, pool, hw, seen, skipped);
    }

    long long candTotal = 0, allFiles = 0;
    std::vector<DupGroup>* out = MakeGroups(pool, hw, candTotal, allFiles);
    pool.clear();

    g_seenFiles = seen; g_skippedFiles = skipped;      // читаются в FinishScan после DONE
    PostMessageW(hw, WM_APP_DONE, (WPARAM)out, 0);
    delete ctx;
    return 0;
}

// ── Корзина и проводник ────────────────────────────────────────────
static bool RecycleFile(const std::wstring& file)
{
    SHFILEOPSTRUCTW op = {};
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    std::wstring from = file; from += L'\0'; from += L'\0';
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
static void OpenInExplorer(const std::wstring& file)
{
    std::wstring dir = DirName(file);
    std::wstring params = L"/select,\"" + file + L"\"";
    INT_PTR r = (INT_PTR)ShellExecuteW(g_hwnd, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if (r <= 32)   // не удалось выделить файл — просто открываем папку
        ShellExecuteW(g_hwnd, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
static std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    wchar_t* p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) { r = p; CoTaskMemFree(p); }
    return r;
}

// ═══════════════════════════════════════════════════════════════════
//  Шрифты
// ═══════════════════════════════════════════════════════════════════
static HFONT MakeFont(int h, const wchar_t* face, int weight = FW_NORMAL)
{
    return CreateFontW(-S(h), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}
static void InitFonts()
{
    g_fLogo  = MakeFont(16, L"Segoe UI Semibold", FW_SEMIBOLD);
    g_fTitle = MakeFont(15, L"Segoe UI Semibold", FW_SEMIBOLD);
    g_fBold  = MakeFont(13, L"Segoe UI Semibold", FW_SEMIBOLD);
    g_fNorm  = MakeFont(13, L"Segoe UI");
    g_fSmall = MakeFont(12, L"Segoe UI");
    g_fTiny  = MakeFont(10, L"Segoe UI Semibold", FW_SEMIBOLD);
    g_fKpi   = MakeFont(19, L"Segoe UI Semibold", FW_SEMIBOLD);
    g_fMono  = MakeFont(12, L"Consolas");
}
static void FreeFonts()
{
    HFONT fs[] = { g_fLogo, g_fTitle, g_fBold, g_fNorm, g_fSmall, g_fTiny, g_fKpi, g_fMono };
    for (HFONT f : fs) if (f) DeleteObject(f);
    g_fLogo = g_fTitle = g_fBold = g_fNorm = g_fSmall = g_fTiny = g_fKpi = g_fMono = nullptr;
}

// ═══════════════════════════════════════════════════════════════════
//  Кнопки
// ═══════════════════════════════════════════════════════════════════
static void SetBtn(int id, const wchar_t* label, IconId ic, int style, bool enabled, RECT rc,
                   const std::wstring& hint)
{
    Btn& b = g_btns[id];
    b.label = label; b.icon = ic; b.style = style; b.enabled = enabled;
    b.rc = rc; b.visible = true; b.hint = hint;
    if (!enabled) b.aHov = MinF(b.aHov, 0.35f);
}
static void HideBtn(int id) { g_btns[id].visible = false; g_btns[id].rc = {}; }

static void DrawBtn(HDC dc, Btn& b)
{
    if (!b.visible || !ValidRc(b.rc)) return;
    const RECT& r = b.rc;
    float rad = (float)S(9);
    float hov = b.aHov, prs = b.aPress;

    COLORREF cTop, cBot, cBorder, cText;
    double glow = 0;
    if (!b.enabled)
    {
        cTop = C_DISABLED; cBot = RGB(26, 31, 38); cBorder = RGB(38, 44, 52); cText = RGB(92, 101, 116);
    }
    else if (b.style == 1)
    {
        cTop  = Mix(C_ACCENT_HI, C_ACCENT, 0.30 + 0.40 * hov);
        cBot  = Mix(C_ACCENT, C_ACCENT_LO, 0.20 + 0.40 * hov);
        cBorder = Mix(cTop, RGB(255, 255, 255), 0.10 + 0.12 * hov);
        cText = RGB(255, 255, 255);
        glow = 0.28 + 0.40 * hov;
    }
    else if (b.style == 2)
    {
        cTop  = Mix(C_DANGER_HI, C_DANGER, 0.30 + 0.40 * hov);
        cBot  = Mix(C_DANGER, RGB(140, 28, 28), 0.15 + 0.40 * hov);
        cBorder = Mix(cTop, RGB(255, 255, 255), 0.08 + 0.12 * hov);
        cText = RGB(255, 255, 255);
        glow = 0.20 + 0.32 * hov;
    }
    else
    {
        cTop  = Mix(C_PANEL_TOP, C_HOVER, 0.20 + 0.85 * hov);
        cBot  = Mix(C_PANEL, C_HOVER, 0.10 + 0.75 * hov);
        cBorder = Mix(C_BORDER, C_BORDER_HI, 0.20 + 0.85 * hov);
        cText = Mix(C_MUTED, C_FG, 0.35 + 0.65 * hov);
        glow = 0.05 * hov;
    }
    if (prs > 0.01f) { cTop = Mix(cTop, RGB(6, 9, 13), 0.38 * prs); cBot = Mix(cBot, RGB(6, 9, 13), 0.38 * prs); }

    RECT sh = { r.left + S(1), r.top + S(2), r.right + S(1), r.bottom + S(2) };
    FillSolid(sh, rad, RGB(0, 0, 0), b.enabled ? (0.28 + 0.16 * hov) : 0.16);
    if (glow > 0.02)
    {
        RECT gl = { r.left - S(6), r.top - S(5), r.right + S(6), r.bottom + S(8) };
        Glow(gl, rad + S(9), b.style == 2 ? C_DANGER : C_ACCENT, glow * 0.5);
    }
    FillRound(r, rad, cTop, cBot);
    StrokeRound(r, rad, cBorder, 1.0, 1.f);
    LineA((float)r.left + rad, (float)r.top + 1.f, (float)r.right - rad, (float)r.top + 1.f,
          RGB(255, 255, 255), b.enabled ? 0.09 + 0.05 * hov : 0.03, 1.f);

    int icW = b.icon ? S(17) : 0;
    int txW = b.label.empty() ? 0 : MeasW(b.label, g_fNorm);
    int gap = (icW && txW) ? S(8) : 0;
    int cx = (r.left + r.right - (icW + gap + txW)) / 2;
    int yc = (r.top + r.bottom) / 2;
    if (icW) DrawIcon(b.icon, cx + icW / 2, yc, icW, cText, 1.0, g_time * 6.f);
    if (txW) Txt(dc, b.label, VR(cx + icW + gap, r.right, yc, S(11)), cText, g_fNorm);
}

// ═══════════════════════════════════════════════════════════════════
//  Раскладка
// ═══════════════════════════════════════════════════════════════════
static int RowH(const Row& r)
{
    return r.kind == Row::Group ? ROW_GROUP : (r.kind == Row::File ? ROW_FILE : ROW_SPACER);
}

static void Layout()
{
    if (!g_hwnd) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    g_wndW = rc.right; g_wndH = rc.bottom;
    int W = g_wndW;
    ComputeSizes();

    g_rHeader  = { 0, 0, W, H_HEADER };
    g_rToolbar = { 0, H_HEADER, W, H_HEADER + H_TOOLBAR };

    // ── панель пути ──
    int tbY = g_rToolbar.top + (H_TOOLBAR - BTN_H) / 2;
    int brwW    = MeasW(L"Обзор…", g_fNorm) + S(28) + S(20);
    int scanW   = MeasW(L"Сканировать", g_fNorm) + S(28) + S(20);
    int cancelW = MeasW(L"Отмена", g_fNorm) + S(28) + S(20);
    int filtW   = MeasW(L"Фильтры", g_fNorm) + S(28) + S(20);

    int x = W - PAD;
    RECT rcCancel = { x - cancelW, tbY, x, tbY + BTN_H };
    if (g_scanning) x -= cancelW + S(8);
    RECT rcScan   = { x - scanW, tbY, x, tbY + BTN_H };
    if (!g_scanning) x -= scanW + S(8);
    RECT rcBrowse = { x - brwW, tbY, x, tbY + BTN_H };
    x -= brwW + S(8);
    RECT rcFilter = { x - filtW, tbY, x, tbY + BTN_H };
    x -= filtW + S(14);

    g_rEdit   = { PAD, tbY + (BTN_H - EDIT_H) / 2, x, tbY + (BTN_H - EDIT_H) / 2 + EDIT_H };
    int textX = g_rEdit.left + S(38);
    int clW   = S(22);
    g_rClear  = { g_rEdit.right - clW - S(10), g_rEdit.top + (EDIT_H - clW) / 2,
                  g_rEdit.right - S(10),       g_rEdit.top + (EDIT_H + clW) / 2 };
    if (g_edit)
        MoveWindow(g_edit, textX, g_rEdit.top + 1,
                   MaxI(S(40), g_rClear.left - textX - S(4)), EDIT_H - 2, TRUE);

    SetBtn(B_BROWSE, L"Обзор…", IC_FOLDER, 0, !g_scanning, rcBrowse,
           g_edit && GetWindowTextLengthW(g_edit) > 0 ? L"Добавить папку к списку · Ctrl+B"
                                                      : L"Выбрать папку в проводнике · Ctrl+B");
    {
        std::wstring fs = FiltersSummary();
        int manual = ManualOrigCount();
        std::wstring fh = L"Фильтры и правила · Ctrl+F · размер: " + std::wstring(MS_LABEL[g_ms]) +
                          L" · тип: " + std::wstring(TY_LABEL[g_ty]) +
                          L" · дата: " + std::wstring(DF_LABEL[g_df]) +
                          L" · оригинал: " + std::wstring(OR_LABEL[g_or]);
        if (manual) fh += L" (вручную выбрано в " + std::to_wstring(manual) + L" гр.)";
        fh += L" · " + (fs.empty() ? std::wstring(L"фильтров нет") : std::wstring(L"активно: ") + fs);
        SetBtn(B_FILTERS, L"Фильтры", IC_FILTER, FiltersActive() ? 1 : 0, !g_scanning, rcFilter, fh);
    }
    SetBtn(B_SCAN,   L"Сканировать", IC_SEARCH, 1, !g_scanning, rcScan, L"Найти дубликаты · F5");
    SetBtn(B_CANCEL, L"Отмена", IC_STOP, 0, g_scanning, rcCancel, L"Прервать сканирование · Esc");
    if (!g_scanning) HideBtn(B_CANCEL);
    else HideBtn(B_SCAN);

    int y = g_rToolbar.bottom;

    // ── быстрый выбор папок (пока нет результатов) ──
    bool showChips = !g_chips.empty() && g_rows.empty() && !g_scanning;
    if (showChips)
    {
        g_rChips = { 0, y, W, y + H_CHIPS };
        y = g_rChips.bottom;
        g_chipsX0 = PAD + TrackedWidth(L"БЫСТРЫЙ ВЫБОР", g_fTiny, S(1)) + S(16);
        int cx = g_chipsX0, chH = S(30), cy = g_rChips.top + H_CHIPS / 2;
        for (auto& c : g_chips)
        {
            int w = MeasW(c.title, g_fSmall) + S(30) + S(12);
            c.rc = { cx, cy - chH / 2, cx + w, cy + chH / 2 };
            cx += w + S(8);
        }
    }
    else { g_rChips = { 0, y, W, y }; }

    // ── статус ──
    g_rStatus = { 0, y, W, y + H_STATUS };
    int prW = S(220), prH = S(6);
    g_rProgress = { W - PAD - prW, g_rStatus.top + (H_STATUS - prH) / 2,
                    W - PAD,        g_rStatus.top + (H_STATUS + prH) / 2 };
    y = g_rStatus.bottom;

    // ── список ──
    g_rCols   = { 0, y, W, y + H_COLHDR };
    y = g_rCols.bottom;
    g_rList   = { 0, y, W, g_wndH - H_FOOTER - 1 };
    g_rFooter = { 0, g_wndH - H_FOOTER, W, g_wndH };

    int fy = g_rFooter.top + (H_FOOTER - BTN_H) / 2;
    int delW  = MeasW(L"Удалить в корзину", g_fNorm) + S(28) + S(20);
    int autW  = MeasW(L"Выбрать дубликаты", g_fNorm) + S(28) + S(20);
    int unW   = MeasW(L"Снять всё", g_fNorm) + S(28) + S(20);
    x = W - PAD;
    RECT rDel = { x - delW, fy, x, fy + BTN_H }; x -= delW + S(8);
    RECT rAut = { x - autW, fy, x, fy + BTN_H }; x -= autW + S(8);
    RECT rUn  = { x - unW,  fy, x, fy + BTN_H };

    bool hasRes = !g_rows.empty();
    SetBtn(B_DELETE, L"Удалить в корзину", IC_TRASH, 2, hasRes && g_checkedCount > 0 && !g_scanning, rDel,
           L"Помеченные копии уйдут в корзину Windows · Del");
    SetBtn(B_AUTO, L"Выбрать дубликаты", IC_CHECKLIST, 0, hasRes && !g_scanning, rAut,
           L"Отметить все копии, кроме оригиналов · Ctrl+A");
    SetBtn(B_UNCHECK, L"Снять всё", IC_LIST, 0, hasRes && !g_scanning, rUn, L"Снять все отметки · Ctrl+D");

    // ── прокрутка ──
    int listH = g_rList.bottom - g_rList.top;
    int totalH = ROW_TOP;
    for (auto& r : g_rows) totalH += RowH(r);
    totalH += S(16);
    g_scrollMax = totalH > listH ? totalH - listH : 0;
    g_scrollTarget = (float)ClampD(g_scrollTarget, 0, g_scrollMax);
    g_scroll       = (float)ClampD(g_scroll, 0, g_scrollMax);
}

struct Cols { int check, chip, name, dir, size, cardL, cardR; };
static Cols GetCols()
{
    int sbW = g_scrollMax > 0 ? S(16) : 0;
    Cols c;
    c.cardL = CARD_X;
    c.cardR = g_wndW - sbW - CARD_X;
    c.check = c.cardL + S(14);
    c.chip  = c.check + CB_SZ + S(11);
    c.name  = c.chip + S(70);
    c.size  = c.cardR - S(14);
    int mid = c.name + (int)((c.size - S(90) - c.name) * 0.50);
    c.dir = MaxI(mid, c.name + S(140));
    return c;
}

// ═══════════════════════════════════════════════════════════════════
//  Отрисовка: шапка окна
// ═══════════════════════════════════════════════════════════════════
static void DrawHeader(HDC dc)
{
    FillRound(g_rHeader, 0, C_PANEL_TOP, C_PANEL);

    // значок
    RECT badge = { PAD, (H_HEADER - BADGE) / 2, PAD + BADGE, (H_HEADER + BADGE) / 2 };
    FillSolid({ badge.left + S(1), badge.top + S(2), badge.right + S(1), badge.bottom + S(2) },
              (float)S(10), RGB(0, 0, 0), 0.35);
    FillRound(badge, (float)S(10), C_ACCENT_HI, C_ACCENT_LO);
    StrokeRound(badge, (float)S(10), RGB(255, 255, 255), 0.16, 1.f);
    {
        float k = BADGE / 24.f;
        float bx = badge.left + BADGE / 2.f, by = badge.top + BADGE / 2.f;
        auto X = [&](double u) { return bx + (float)((u - 12) * k); };
        auto Y = [&](double u) { return by + (float)((u - 12) * k); };
        RECT b1{ (int)X(8.5), (int)Y(3.5), (int)X(20.5), (int)Y(15.5) };
        FillSolid(b1, 3 * k, RGB(255, 255, 255), 0.45);
        RECT b2{ (int)X(3.5), (int)Y(8), (int)X(15.5), (int)Y(20) };
        FillSolid(b2, 3 * k, RGB(255, 255, 255), 0.95);
        PointF ch[] = { { X(6.2), Y(13.9) }, { X(9.0), Y(16.6) }, { X(12.9), Y(10.8) } };
        PolyA(ch, 3, C_ACCENT_LO, 0.9, (float)MaxI(1, S(2)));
    }

    // словол
    int tx = PAD + BADGE + S(13);
    int ycTop = H_HEADER / 2 - S(11);
    int w1 = MeasW(L"mebo", g_fLogo);
    int w2 = MeasW(L"system", g_fLogo);
    Txt(dc, L"mebo",   VR(tx, tx + w1 + 2, ycTop, S(13)), C_FG, g_fLogo);
    Txt(dc, L"system", VR(tx + w1, tx + w1 + w2 + 2, ycTop, S(13)), C_ACCENT, g_fLogo);
    int capW = TrackedWidth(L"DUPLICATE CLEANER", g_fTiny, S(2));
    TxtTracked(dc, L"DUPLICATE CLEANER", tx, H_HEADER / 2 + S(12), C_FAINT, g_fTiny, S(2));

    // разделитель
    int sepX = tx + MaxI(w1 + w2 + S(2), capW) + S(20);
    LineA((float)sepX, (float)(H_HEADER / 2 - S(19)), (float)sepX, (float)(H_HEADER / 2 + S(19)), C_BORDER, 1.0, 1.f);

    // заголовок + подзаголовок
    int hx = sepX + S(18);
    int rightLimit = g_wndW - PAD - S(150);
    Txt(dc, L"Дубликаты файлов", VR(hx, rightLimit, H_HEADER / 2 - S(10), S(13)), C_FG, g_fTitle);
    Txt(dc, L"Сравнение по содержимому (SHA-256), удаление в корзину",
        VR(hx, rightLimit, H_HEADER / 2 + S(13), S(10)), C_FAINT, g_fSmall);

    // чип версии + состояние
    std::wstring ver = APP_VERSION;
    int vw = MeasW(ver, g_fTiny) + S(18);
    RECT vr = { g_wndW - PAD - vw, H_HEADER / 2 - S(23), g_wndW - PAD, H_HEADER / 2 - S(23) + S(19) };
    FillSolid(vr, (float)S(9), RGB(6, 9, 13), 0.45);
    StrokeRound(vr, (float)S(9), C_BORDER, 0.9, 1.f);
    Txt(dc, ver, vr, C_MUTED, g_fTiny, DT_CENTER);

    COLORREF dotC = g_scanning ? C_AMBER : (!g_rows.empty() ? C_MINT : C_MUTED);
    float pulse = g_scanning ? (float)(0.5 + 0.5 * fabs(sin(g_time * 3.0))) : 1.f;
    std::wstring st = g_scanning ? (g_phase == 0 ? L"перебор файлов" : L"хэширование")
                    : (!g_rows.empty() ? L"результат готов" : L"ожидание папки");
    int sw2 = MeasW(st, g_fTiny);
    DotA((float)(g_wndW - PAD - sw2 - S(12)), (float)(H_HEADER / 2 + S(16)), (float)S(3.2), dotC, 0.55 + 0.45 * pulse);
    Txt(dc, st, VR(g_wndW - PAD - sw2, g_wndW - PAD, H_HEADER / 2 + S(16), S(9)),
        Mix(C_FAINT, dotC, 0.45), g_fTiny, DT_RIGHT);

    // нижняя граница и световая полоса
    RECT gl = { 0, H_HEADER, g_wndW, H_HEADER + S(9) };
    Glow(gl, 0, C_ACCENT, 0.13);
    LineA(0.f, (float)H_HEADER, (float)g_wndW, (float)H_HEADER, C_BORDER, 1.0, 1.f);
    LineA((float)PAD, (float)H_HEADER - 1.f, (float)(g_wndW - PAD), (float)H_HEADER - 1.f, C_ACCENT, 0.5, 1.f);
}

// ═══════════════════════════════════════════════════════════════════
//  Отрисовка: панель пути
// ═══════════════════════════════════════════════════════════════════
static void DrawToolbar(HDC dc)
{
    FillRound(g_rToolbar, 0, C_PANEL, C_BG_TOP);
    LineA(0.f, (float)g_rToolbar.bottom, (float)g_wndW, (float)g_rToolbar.bottom, C_BORDER, 0.85, 1.f);

    float rad = (float)S(10);
    FillSolid(g_rEdit, rad, C_INPUT, 1.0);
    if (g_editFocus)
    {
        RECT ring = { g_rEdit.left - S(4), g_rEdit.top - S(4), g_rEdit.right + S(4), g_rEdit.bottom + S(4) };
        Glow(ring, rad + S(5), C_ACCENT, 0.22);
    }
    StrokeRound(g_rEdit, rad, g_editFocus ? C_ACCENT : C_BORDER, g_editFocus ? 0.95 : 1.0, g_editFocus ? 1.4f : 1.f);

    int yc = (g_rEdit.top + g_rEdit.bottom) / 2;
    DrawIcon(IC_FOLDER, g_rEdit.left + S(18), yc, S(18), g_editFocus ? C_ACCENT : C_FAINT);

    int len = g_edit ? GetWindowTextLengthW(g_edit) : 0;
    if (len == 0)
        Txt(dc, L"Папка для поиска · несколько через | …", VR(g_rEdit.left + S(40), g_rEdit.right - S(10), yc, S(11)),
            C_FAINT, g_fMono, DT_LEFT, g_editFocus ? 0.75 : 1.0);
    else if (g_clearHover)
    {
        FillSolid(g_rClear, (float)S(7), C_HOVER, 0.95);
        DrawIcon(IC_X, (g_rClear.left + g_rClear.right) / 2, (g_rClear.top + g_rClear.bottom) / 2, S(12), C_FG);
    }
    else
        DrawIcon(IC_X, (g_rClear.left + g_rClear.right) / 2, (g_rClear.top + g_rClear.bottom) / 2, S(12), C_FAINT, 0.8);

    DrawBtn(dc, g_btns[B_BROWSE]);
    DrawBtn(dc, g_btns[B_FILTERS]);
    DrawBtn(dc, g_btns[B_SCAN]);
    DrawBtn(dc, g_btns[B_CANCEL]);
}

// ═══════════════════════════════════════════════════════════════════
//  Отрисовка: быстрый выбор
// ═══════════════════════════════════════════════════════════════════
static void DrawChips(HDC dc)
{
    if (!ValidRc(g_rChips)) return;
    FillRound(g_rChips, 0, C_BG_TOP, C_BG);
    TxtTracked(dc, L"БЫСТРЫЙ ВЫБОР", PAD, g_rChips.top + H_CHIPS / 2, C_FAINT, g_fTiny, S(1));
    for (auto& c : g_chips)
    {
        if (!ValidRc(c.rc)) continue;
        float rad = (float)((c.rc.bottom - c.rc.top) / 2);
        FillSolid({ c.rc.left + S(1), c.rc.top + S(2), c.rc.right + S(1), c.rc.bottom + S(2) }, rad, RGB(0, 0, 0), 0.22);
        FillRound(c.rc, rad, Mix(C_PANEL_TOP, C_HOVER, 0.25 + 0.75 * c.aHov),
                           Mix(C_PANEL, C_HOVER, 0.15 + 0.65 * c.aHov));
        StrokeRound(c.rc, rad, Mix(C_BORDER, C_ACCENT, 0.10 + 0.75 * c.aHov), 1.0, 1.f);
        int yc = (c.rc.top + c.rc.bottom) / 2;
        DrawIcon(c.icon, c.rc.left + S(17), yc, S(16), Mix(C_MUTED, C_ACCENT_HI, 0.2 + 0.7 * c.aHov));
        Txt(dc, c.title, VR(c.rc.left + S(30), c.rc.right - S(10), yc, S(10)),
            Mix(C_MUTED, C_FG, 0.25 + 0.75 * c.aHov), g_fSmall);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Отрисовка: статус и прогресс
// ═══════════════════════════════════════════════════════════════════
static std::wstring StatusText()
{
    if (!g_hint.empty()) return g_hint;
    if (g_scanning)
    {
        if (g_phase == 0) return L"Перебор файлов: " + FmtNum(g_filesSeen) + L"…";
        double el = (double)(GetTickCount64() - g_scanStart) / 1000.0;
        double spd = el > 0.3 ? (double)g_progDone / el : 0.0;
        std::wstring s = L"Хэширование " + FmtNum(g_progDone) + L" / " + FmtNum(g_progTotal);
        if (spd > 1)
        {
            s += L"  ·  " + FmtNum((long long)spd) + L" ф/с";
            if (g_progTotal > g_progDone) s += L"  ·  осталось " + FmtTime((double)(g_progTotal - g_progDone) / spd);
        }
        return s;
    }
    return g_status;
}
static void DrawStatus(HDC dc)
{
    FillRound(g_rStatus, 0, C_BG, C_BG_TOP);
    int yc = (g_rStatus.top + g_rStatus.bottom) / 2;

    IconId ic = g_hint.empty() ? (IconId)g_statusIcon : IC_INFO;
    COLORREF c = C_MUTED;
    if (!g_hint.empty()) c = C_FAINT;
    else if (g_scanning) { ic = IC_SPINNER; c = C_ACCENT; }
    else if (ic == IC_OK) c = C_MINT;
    else if (ic == IC_WARN) c = C_AMBER;
    else if (ic == IC_TRASH) c = C_DANGER_HI;

    DrawIcon(ic, PAD + S(9), yc, S(17), c, 1.0, g_time * 5.5f);

    // справа: прогресс или итог
    std::wstring pct;
    int pw = 0;
    int textRight = g_wndW - PAD;
    if (g_scanning)
    {
        textRight = g_rProgress.left - S(16);
        if (g_phase == 1 && g_progTotal > 0)
        {
            wchar_t b[16]; wsprintfW(b, L"%d%%", (int)(ClampD((double)g_progDone / g_progTotal, 0, 1) * 100 + 0.5));
            pct = b; pw = MeasW(pct, g_fTiny);
            textRight = g_rProgress.left - pw - S(26);
        }
        else if (g_phase == 0)
        {
            pct = L"…";
            pw = MeasW(pct, g_fTiny);
            textRight = g_rProgress.left - pw - S(26);
        }
    }

    Txt(dc, StatusText(), VR(PAD + S(28), textRight, yc, S(12)),
        g_hint.empty() ? Mix(C_FG, C_MUTED, 0.30) : c, g_fNorm);

    if (!pct.empty())
        Txt(dc, pct, VR(g_rProgress.left - pw - S(14), g_rProgress.left - S(8), yc, S(9)), C_ACCENT, g_fTiny, DT_RIGHT);

    if (g_scanning)
    {
        float rad = (float)S(3);
        FillSolid(g_rProgress, rad, RGB(7, 10, 14), 0.95);
        StrokeRound(g_rProgress, rad, C_BORDER, 0.75, 1.f);
        if (g_phase == 0)
        {
            int bw = S(64);
            int span = (g_rProgress.right - g_rProgress.left) + bw;
            int pos = (int)((g_marquee / 3) % (span > 0 ? span : 1));
            int bx = g_rProgress.left + pos - bw;
            RECT f = { MaxI(g_rProgress.left, bx), g_rProgress.top,
                       MinI(g_rProgress.right, bx + bw), g_rProgress.bottom };
            if (ValidRc(f)) FillRound(f, rad, C_ACCENT_HI, C_ACCENT);
        }
        else
        {
            double frac = g_progTotal > 0 ? ClampD((double)g_progDone / g_progTotal, 0, 1) : 0;
            int w = (int)((g_rProgress.right - g_rProgress.left) * frac);
            RECT f = { g_rProgress.left, g_rProgress.top, g_rProgress.left + w, g_rProgress.bottom };
            if (ValidRc(f) && f.right > f.left + 1)
            {
                FillRound(f, rad, C_ACCENT_HI, C_ACCENT);
                Glow({ f.left, f.top - S(5), f.right + S(3), f.bottom + S(5) }, rad + S(5), C_ACCENT, 0.30);
                LineA((float)f.right - 1.f, (float)f.top, (float)f.right - 1.f, (float)f.bottom, RGB(255, 255, 255), 0.5, 1.5f);
            }
        }
    }
    else if (!g_rows.empty())
    {
        std::wstring s = L"будет освобождено " + FmtBytes(g_totalFreed);
        int w = MeasW(s, g_fTiny) + S(30);
        RECT pr = { g_wndW - PAD - w, yc - S(11), g_wndW - PAD, yc + S(11) };
        FillSolid(pr, (float)S(11), C_MINT, 0.10);
        StrokeRound(pr, (float)S(11), C_MINT, 0.28, 1.f);
        DotA((float)(pr.left + S(12)), (float)yc, (float)S(2.6), C_MINT, 0.9);
        Txt(dc, s, VR(pr.left + S(21), pr.right - S(8), yc, S(9)), Mix(C_MINT, C_FG, 0.25), g_fTiny);
    }
    else if (FiltersActive())
    {
        // напоминание: без результатов, но фильтры включены — поиск будет выборочным
        std::wstring s = L"фильтры: " + FiltersSummary();
        int w = MeasW(s, g_fTiny) + S(34);
        RECT pr = { g_wndW - PAD - w, yc - S(11), g_wndW - PAD, yc + S(11) };
        FillSolid(pr, (float)S(11), C_AMBER, 0.10);
        StrokeRound(pr, (float)S(11), C_AMBER, 0.32, 1.f);
        DrawIcon(IC_FILTER, pr.left + S(14), yc, S(12), C_AMBER, 0.95);
        Txt(dc, s, VR(pr.left + S(24), pr.right - S(8), yc, S(9)), Mix(C_AMBER, C_FG, 0.25), g_fTiny);
    }
    LineA(0.f, (float)g_rStatus.bottom, (float)g_wndW, (float)g_rStatus.bottom, C_BORDER, 0.7, 1.f);
}

// ═══════════════════════════════════════════════════════════════════
//  Отрисовка: список
// ═══════════════════════════════════════════════════════════════════
static void DrawColHdr(HDC dc)
{
    FillRound(g_rCols, 0, C_BG_TOP, C_BG);
    if (g_rows.empty() && !g_scanning) { LineA(0.f, (float)g_rCols.bottom, (float)g_wndW, (float)g_rCols.bottom, C_BORDER, 0.5, 1.f); return; }
    int yc = g_rCols.top + H_COLHDR / 2;
    Cols cl = GetCols();
    TxtTracked(dc, L"ФАЙЛ", cl.name, yc, C_MUTED, g_fTiny, S(1));
    TxtTracked(dc, L"РАСПОЖЕНИЕ", cl.dir, yc, C_FAINT, g_fTiny, S(1));
    TxtTracked(dc, L"РАЗМЕР", cl.size, yc, C_MUTED, g_fTiny, S(1), true);
    TxtTracked(dc, L"ГРУППЫ И КОПИИ", cl.cardL + S(14), yc, C_FAINT, g_fTiny, S(1));
    LineA(0.f, (float)g_rCols.bottom, (float)g_wndW, (float)g_rCols.bottom, C_BORDER, 0.9, 1.f);
}

struct Seg { int idx, y, h; };
static void BuildSegs(std::vector<Seg>& segs)
{
    int y = g_rList.top + ROW_TOP - (int)g_scroll;
    for (int i = 0; i < (int)g_rows.size(); ++i)
    {
        int h = RowH(g_rows[i]);
        if (y > g_rList.bottom) break;
        if (y + h > g_rList.top) segs.push_back({ i, y, h });
        y += h;
    }
}

static void DrawEmptyState(HDC dc)
{
    int cx = g_wndW / 2;
    int cy = g_rList.top + (g_rList.bottom - g_rList.top) / 2;
    Glow({ cx - S(170), cy - S(150), cx + S(170), cy + S(130) }, S(140),
         g_scanning ? C_ACCENT : C_ACCENT_LO, 0.09);

    if (g_scanning)
    {
        int iy = cy - S(70);
        for (int i = 0; i < 5; ++i)
        {
            int yy = iy + i * S(28);
            float ph = (float)(0.5 + 0.5 * sin(g_time * 2.4 + i * 0.7));
            COLORREF c = Mix(RGB(24, 30, 38), RGB(46, 57, 72), ph * 0.9);
            FillSolid({ cx - S(210), yy, cx - S(90), yy + S(14) }, (float)S(5), c, 0.95);
            FillSolid({ cx - S(78), yy + S(3), cx + S(110), yy + S(11) }, (float)S(4), c, 0.55);
            FillSolid({ cx + S(140), yy, cx + S(210), yy + S(14) }, (float)S(5), c, 0.7);
        }
        DrawIcon(IC_SPINNER, cx - S(246), iy + S(38), S(30), C_ACCENT, 0.95, g_time * 4.f);
        Txt(dc, L"Ищем одинаковые файлы…", VR(cx - S(230), cx + S(230), iy + S(178), S(13)), C_FG, g_fTitle, DT_CENTER);
        Txt(dc, L"Ничего не изменяется и не удаляется — только чтение",
            VR(cx - S(230), cx + S(230), iy + S(204), S(10)), C_FAINT, g_fSmall, DT_CENTER);
        return;
    }

    int dx = cx - S(36), dy = cy - S(110);
    RECT b1{ dx, dy, dx + S(52), dy + S(64) };
    FillSolid(b1, (float)S(9), C_PANEL_TOP, 0.95);
    StrokeRound(b1, (float)S(9), C_BORDER, 1.0, 1.4f);
    RECT b2{ dx + S(22), dy + S(14), dx + S(74), dy + S(78) };
    FillSolid(b2, (float)S(9), C_CARD_TOP, 1.0);
    StrokeRound(b2, (float)S(9), C_BORDER_HI, 1.0, 1.4f);
    for (int i = 0; i < 3; ++i)
        LineA((float)(b2.left + S(10)), (float)(b2.top + S(18) + i * S(12)),
              (float)(b2.right - S(10) - (i == 2 ? S(16) : 0)), (float)(b2.top + S(18) + i * S(12)),
              C_FAINT, 0.55, (float)S(2));
    DrawIcon(IC_SEARCH, dx + S(76), dy + S(74), S(34), C_ACCENT, 0.95);

    Txt(dc, L"Пока ничего не найдено", VR(cx - S(280), cx + S(280), dy + S(116), S(14)), C_FG, g_fTitle, DT_CENTER);
    Txt(dc, L"Выберите папку и нажмите «Сканировать» — файлы сравниваются по содержимому.",
        VR(cx - S(300), cx + S(300), dy + S(144), S(11)), C_MUTED, g_fNorm, DT_CENTER);
    Txt(dc, L"Несколько папок за пуск — через | . Оригиналы остаются, копии помечаются для удаления в корзину.",
        VR(cx - S(300), cx + S(300), dy + S(168), S(10)), C_FAINT, g_fSmall, DT_CENTER);
    if (FiltersActive())
        Txt(dc, L"Фильтры: " + FiltersSummary() + L" · оригинал: " + std::wstring(OR_LABEL[g_or]),
            VR(cx - S(300), cx + S(300), dy + S(192), S(10)), C_AMBER, g_fSmall, DT_CENTER);
}

static void DrawList(HDC dc)
{
    if (g_rows.empty()) { DrawEmptyState(dc); return; }

    HRGN prevClip = CreateRectRgn(0, 0, 0, 0);
    int hadClip = GetClipRgn(dc, prevClip);
    HRGN lr = CreateRectRgn(g_rList.left, g_rList.top, g_rList.right, g_rList.bottom);
    SelectClipRgn(dc, lr);
    DeleteObject(lr);

    std::vector<Seg> segs;
    BuildSegs(segs);
    Cols cl = GetCols();

    // ── карточки групп ──
    for (size_t i = 0; i < segs.size(); ++i)
    {
        if (g_rows[segs[i].idx].kind != Row::Group) continue;
        int top = segs[i].y - CARD_PAD;
        int bot = segs[i].y + segs[i].h + CARD_PAD;
        size_t j = i + 1;
        while (j < segs.size() && g_rows[segs[j].idx].kind == Row::File) { bot = segs[j].y + segs[j].h + CARD_PAD; ++j; }
        RECT card = { cl.cardL, top, cl.cardR, bot };
        double a = g_reveal;
        FillSolid({ card.left + S(1), card.top + S(3), card.right + S(1), card.bottom + S(3) },
                  (float)S(13), RGB(0, 0, 0), 0.26 * a);
        FillRound(card, (float)S(13), Mix(C_CARD_TOP, C_BG, 1 - a), Mix(C_CARD, C_BG, 1 - a));
        StrokeRound(card, (float)S(13), C_BORDER, 0.9 * a, 1.f);
        RECT spine = { card.left + 1, card.top + S(11), card.left + S(4), card.bottom - S(11) };
        FillSolid(spine, (float)S(2), C_ACCENT, 0.92 * a);
        Row& gr = g_rows[segs[i].idx];
        double share = ClampD((double)gr.freeBytes / (double)(g_maxFree > 0 ? g_maxFree : 1), 0.05, 1.0);
        RECT sb = { card.left + S(4), card.top + 1,
                    card.left + S(4) + (int)((card.right - card.left) * share * 0.6), card.top + S(3) };
        FillSolid(sb, (float)S(2), C_MINT, 0.40 * a);
        i = j - 1;
    }

    // ── содержимое строк ──
    int vis = 0;
    for (auto& sg : segs)
    {
        Row& r = g_rows[sg.idx];
        if (r.kind == Row::Spacer) { ++vis; continue; }
        double t = ClampD((g_reveal - MinI(vis, 14) * 0.045) * 6.0, 0.0, 1.0);
        ++vis;
        if (t <= 0.005) continue;
        int yc = sg.y + sg.h / 2 + (int)((1.0 - t) * S(7));
        double hv = 0;
        if (sg.idx == g_hoverRow) hv = g_rowHov;
        else if (sg.idx == g_prevHoverRow) hv = 1.0 - g_rowHov;

        if (r.kind == Row::Group)
        {
            RECT hl = { cl.cardL + S(6), sg.y + S(1), cl.cardR - S(6), sg.y + sg.h - S(1) };
            if (hv > 0.01) FillSolid(hl, (float)S(7), C_HOVER, 0.5 * hv);
            DrawIcon(IC_COPIES, cl.check + CB_SZ / 2, yc, S(17), Mix(C_ACCENT, C_FG, 0.2), t);
            Txt(dc, r.text, VR(cl.chip, cl.size - S(160), yc, S(12)), Mix(C_FG, C_CARD, 1 - t), g_fBold);
            std::wstring fr = L"освободится " + FmtBytes(r.freeBytes);
            int fw = MeasW(fr, g_fTiny) + S(24);
            RECT pill = { cl.size - S(12) - fw, yc - S(10), cl.size - S(12), yc + S(10) };
            FillSolid(pill, (float)S(10), C_MINT, 0.12 * t);
            StrokeRound(pill, (float)S(10), C_MINT, 0.30 * t, 1.f);
            Txt(dc, fr, pill, Mix(C_MINT, C_CARD, 1 - t), g_fTiny, DT_CENTER);
            continue;
        }
        if (r.kind != Row::File) continue;

        if (hv > 0.01)
        {
            RECT hl = { cl.cardL + S(6), sg.y + S(1), cl.cardR - S(6), sg.y + sg.h - S(1) };
            FillSolid(hl, (float)S(7), C_HOVER, 0.55 * hv);
        }

        bool isOriginal = r.indexInGroup == 0;
        float ck = r.aCheck;
        float cbx = (float)cl.check, cby = (float)yc;

        if (isOriginal)
        {
            DotA(cbx + CB_SZ / 2.f, cby, CB_SZ / 2.f + S(2), C_MINT, 0.13 * t);
            RingA(cbx + CB_SZ / 2.f, cby, CB_SZ / 2.f, C_MINT, 0.7 * t, (float)MaxI(1, S(1)));
            PointF ch[] = { { cbx + CB_SZ * 0.28f, cby + CB_SZ * 0.02f },
                            { cbx + CB_SZ * 0.45f, cby + CB_SZ * 0.22f },
                            { cbx + CB_SZ * 0.74f, cby - CB_SZ * 0.20f } };
            PolyA(ch, 3, C_MINT, 0.95 * t, (float)MaxI(1, S(1.6)));
        }
        else
        {
            RECT cb = { cl.check, yc - CB_SZ / 2, cl.check + CB_SZ, yc + CB_SZ / 2 };
            FillSolid(cb, (float)S(5), Mix(C_INPUT, C_ACCENT, 0.85 * ck), t);
            StrokeRound(cb, (float)S(5), Mix(C_BORDER_HI, C_ACCENT_HI, ck), (0.9 - 0.35 * ck) * t, 1.f);
            if (ck > 0.02f)
            {
                PointF ch[] = { { (float)cb.left + CB_SZ * 0.24f, (float)yc + CB_SZ * 0.02f },
                                { (float)cb.left + CB_SZ * 0.42f, (float)yc + CB_SZ * 0.25f },
                                { (float)cb.left + CB_SZ * 0.78f, (float)yc - CB_SZ * 0.22f } };
                PolyA(ch, 3, RGB(255, 255, 255), t * ck, (float)MaxI(1, S(2)));
            }
        }

        std::wstring chip = isOriginal ? L"ОРИГИНАЛ" : L"ДУБЛИКАТ";
        COLORREF chipC = isOriginal ? C_MINT : C_FAINT;
        int chipW = TrackedWidth(chip, g_fTiny, S(1));
        RECT cr = { cl.chip, yc - S(8), cl.chip + chipW, yc + S(8) };
        FillSolid(cr, (float)S(5), chipC, (isOriginal ? 0.13 : 0.07) * t);
        TxtTracked(dc, chip, cr.left, yc, Mix(chipC, C_CARD, 1 - t), g_fTiny, S(1), false, t);

        COLORREF nameC = isOriginal ? C_FG : (r.checked ? Mix(C_FG, C_MUTED, 0.35) : C_MUTED);
        if (!isOriginal && hv > 0.4) nameC = C_FG;
        Txt(dc, r.name, VR(cl.name, cl.dir - S(16), yc, S(12)), Mix(nameC, C_CARD, 1 - t), g_fNorm, DT_LEFT, t);
        Txt(dc, r.dir, VR(cl.dir, cl.size - S(90), yc, S(12)), Mix(C_FAINT, C_CARD, 1 - t), g_fMono, DT_PATH_ELLIPSIS, t);
        Txt(dc, FmtBytes(r.size), VR(cl.size - S(86), cl.size - S(12), yc, S(12)),
            Mix(r.checked ? C_ACCENT : C_MUTED, C_CARD, 1 - t), g_fMono, DT_RIGHT, t);
    }

    if (hadClip == 1) SelectClipRgn(dc, prevClip); else SelectClipRgn(dc, nullptr);
    DeleteObject(prevClip);
}

static void DrawScrollBar()
{
    if (g_scrollMax <= 0) return;
    int listH = g_rList.bottom - g_rList.top;
    int total = g_scrollMax + listH;
    int thumbH = MaxI(listH * listH / total, S(36));
    int trackY = g_rList.top + S(4);
    int trackH = listH - S(8);
    int thumbY = trackY + (int)((trackH - thumbH) * (g_scroll / (double)g_scrollMax));
    int x0 = g_wndW - S(11), x1 = g_wndW - S(4);
    bool on = g_thumbHot || g_thumbDrag;
    RECT track = { x0, trackY, x1, trackY + trackH };
    FillSolid(track, (float)S(4), RGB(0, 0, 0), 0.20);
    RECT th = { x0, thumbY, x1, MinI(trackY + trackH, thumbY + thumbH) };
    FillSolid(th, (float)S(4), Mix(C_BORDER_HI, C_ACCENT, on ? 0.55 : 0.0), on ? 0.98 : 0.55);
}

// ═══════════════════════════════════════════════════════════════════
//  Отрисовка: подвал
// ═══════════════════════════════════════════════════════════════════
static void DrawFooter(HDC dc)
{
    FillRound(g_rFooter, 0, C_PANEL_TOP, C_PANEL);
    LineA(0.f, (float)g_rFooter.top, (float)g_wndW, (float)g_rFooter.top, C_BORDER, 1.0, 1.f);
    int yc = g_rFooter.top + H_FOOTER / 2;

    struct Tile { std::wstring cap, val; COLORREF c; };
    Tile tiles[4] = {
        { L"ГРУППЫ",   FmtNum((long long)g_groups.size()), C_FG },
        { L"ФАЙЛОВ",   FmtNum(g_totalFiles), C_FG },
        { L"ОСВОБОДИТСЯ", g_groups.empty() ? L"0 Б" : FmtBytes(g_totalFreed), C_MINT },
        { L"ПОМЕЧЕНО", g_checkedCount ? (FmtNum(g_checkedCount) + L" · " + FmtBytes(g_checkedBytes)) : L"—", C_ACCENT },
    };
    int widths[4], need = 0;
    for (int i = 0; i < 4; ++i)
    {
        widths[i] = MaxI(TrackedWidth(tiles[i].cap, g_fTiny, S(1)), MeasW(tiles[i].val, g_fKpi)) + S(26);
        need += widths[i] + S(1);
    }
    int btnLeft = ValidRc(g_btns[B_UNCHECK].rc) ? g_btns[B_UNCHECK].rc.left : g_wndW - PAD;
    int avail = btnLeft - PAD - S(20);
    int skip = 0;
    while (need > avail && skip < 3) { need -= widths[skip] + S(1); ++skip; }

    int x = PAD;
    for (int i = skip; i < 4; ++i)
    {
        if (i > skip) { LineA((float)x, (float)(yc - S(19)), (float)x, (float)(yc + S(19)), C_BORDER, 1.0, 1.f); x += S(1); }
        TxtTracked(dc, tiles[i].cap, x + S(13), yc - S(15), C_FAINT, g_fTiny, S(1));
        Txt(dc, tiles[i].val, VR(x + S(13), x + widths[i] - S(10), yc + S(12), S(15)), tiles[i].c, g_fKpi);
        x += widths[i] + S(1);
    }

    DrawBtn(dc, g_btns[B_UNCHECK]);
    DrawBtn(dc, g_btns[B_AUTO]);
    DrawBtn(dc, g_btns[B_DELETE]);
}

// ═══════════════════════════════════════════════════════════════════
//  Кадр
// ═══════════════════════════════════════════════════════════════════
static void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 1 || rc.bottom <= 1) { EndPaint(hwnd, &ps); return; }

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP oldb = (HBITMAP)SelectObject(mem, bmp);

    Graphics gx(mem);
    gx.SetSmoothingMode(SmoothingModeAntiAlias);
    gx.SetPixelOffsetMode(PixelOffsetModeHalf);
    g_gx = &gx;

    FillRound({ 0, 0, rc.right, rc.bottom }, 0, C_BG_TOP, RGB(10, 13, 18));
    Glow({ 0, 0, rc.right, H_HEADER + S(140) }, 0, C_ACCENT, 0.05);

    DrawStatus(mem);
    DrawChips(mem);
    DrawColHdr(mem);
    DrawList(mem);
    DrawScrollBar();
    DrawFooter(mem);
    DrawToolbar(mem);
    DrawHeader(mem);

    g_gx = nullptr;
    gx.Flush();

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldb);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ═══════════════════════════════════════════════════════════════════
//  Данные
// ═══════════════════════════════════════════════════════════════════
static void RecountChecked()
{
    g_checkedBytes = 0; g_checkedCount = 0;
    for (auto& r : g_rows) if (r.kind == Row::File && r.checked) { g_checkedBytes += r.size; ++g_checkedCount; }
}
static void BuildRows()
{
    g_rows.clear();
    g_checkedBytes = 0; g_checkedCount = 0; g_totalFiles = 0;
    g_totalFreed = 0; g_maxFree = 1;
    for (auto& g : g_groups)
    {
        long long fr = g.size * ((long long)g.files.size() - 1);
        g_totalFreed += fr;
        if (fr > g_maxFree) g_maxFree = fr;
    }
    int gi = 0;
    for (auto& g : g_groups)
    {
        if (!g_rows.empty()) { Row sp; sp.kind = Row::Spacer; g_rows.push_back(sp); }
        Row gr; gr.kind = Row::Group; gr.group = gi;
        long long dups = (long long)g.files.size() - 1;
        gr.text = std::to_wstring(g.files.size()) + L" " +
                  PluralRu((long long)g.files.size(), L"файл", L"файла", L"файлов") +
                  L" по " + FmtBytes(g.size) + L" · " +
                  std::to_wstring(dups) + L" " + PluralRu(dups, L"копия", L"копии", L"копий");
        gr.freeBytes = g.size * ((long long)g.files.size() - 1);
        g_rows.push_back(gr);
        for (size_t i = 0; i < g.files.size(); ++i)
        {
            Row fr; fr.kind = Row::File;
            fr.path = g.files[i].path;
            fr.name = BaseName(g.files[i].path);
            fr.dir  = DirName(g.files[i].path);
            fr.mtime = g.files[i].mtime;
            fr.size = g.size;
            fr.group = gi;
            fr.indexInGroup = (int)i;
            fr.checked = i > 0;
            fr.aCheck = fr.checked ? 1.f : 0.f;
            if (fr.checked) { g_checkedBytes += fr.size; ++g_checkedCount; }
            ++g_totalFiles;
            g_rows.push_back(fr);
        }
        ++gi;
    }
    g_reveal = 0.f;
}
// то же, но отметки пользователя переносятся: снимаем список «не отмеченных копий»
static void BuildRowsKeepChecks()
{
    std::set<std::wstring> unmarked;
    for (auto& r : g_rows)
        if (r.kind == Row::File && r.indexInGroup > 0 && !r.checked) unmarked.insert(r.path);
    BuildRows();
    if (unmarked.empty()) return;
    for (auto& r : g_rows)
        if (r.kind == Row::File && r.indexInGroup > 0 && unmarked.count(r.path))
            { r.checked = false; r.aCheck = 0.f; }
    RecountChecked();
}
// «сырой» результат → список с учётом фильтров (группа остаётся, если в ней ≥2 файла)
// «сырой» результат → список с учётом фильтров (группа остаётся, если в ней ≥2 файла)
static void RebuildFromRaw()
{
    RefreshExtSet(); RefreshDateWindow();
    std::vector<DupGroup> kept;
    for (auto& src : g_allGroups)
    {
        DupGroup g; g.size = src.size; g.hash = src.hash; g.manualOrig = src.manualOrig;
        for (auto& f : src.files)
            if (PassSize(src.size) && PassType(f.path) && PassDate(f.mtime)) g.files.push_back(f);
        if (g.files.size() < 2) continue;
        size_t hand = std::wstring::npos;
        if (!g.manualOrig.empty())
            for (size_t i = 0; i < g.files.size(); ++i)
                if (g.files[i].path == g.manualOrig) { hand = i; break; }
        if (hand != std::wstring::npos)
        {
            DupFile f = g.files[hand];
            g.files.erase(g.files.begin() + hand);
            g.files.insert(g.files.begin(), f);
        }
        else ApplyOrigRule(g);
        kept.push_back(g);
    }
    g_groups = kept;
    BuildRowsKeepChecks();
    Layout();
}
// фильтр поправил состав групп — пересобираем без потери отметок
static void OnFilterChanged()
{
    if (g_scanning) return;
    if (!g_allGroups.empty()) RebuildFromRaw();
    else Layout();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static std::wstring ResultStatus()
{
    std::wstring where = CurrentRootLabel();
    std::wstring head = where.empty() ? std::wstring() : L"«" + where + L"»: ";
    if (g_allGroups.empty())
        return head + (g_groups.empty() ? std::wstring(L"Дубликатов не найдено — в папке чисто.")
                                        : std::wstring(L"После фильтров групп не осталось."));
    std::wstring s = head + L"групп — " + std::to_wstring(g_groups.size());
    if (g_groups.size() != g_allGroups.size())
        s += L" из " + std::to_wstring(g_allGroups.size()) + L" (фильтр)";
    s += L" · копии помечены — можно удалять";
    return s;
}
static void SetStatus(const std::wstring& s, IconId ic) { g_status = s; g_statusIcon = ic; }
static std::wstring GetEditPath()
{
    if (!g_edit) return L"";
    int len = GetWindowTextLengthW(g_edit);
    std::wstring root(len + 1, L'\0');
    GetWindowTextW(g_edit, &root[0], len + 1);
    root.resize(len);
    while (!root.empty() && (root.back() == L'"' || root.back() == L' ')) root.pop_back();
    return root;
}

// ═══════════════════════════════════════════════════════════════════
//  Анимация
// ═══════════════════════════════════════════════════════════════════
static float EaseTo(float& v, float target, float rate, float dt)
{
    float d = target - v;
    if (fabsf(d) < 0.005f) { v = target; return 0.f; }
    v += d * MinF(1.f, rate * dt);
    return fabsf(target - v);
}
static bool TickAnim(float dt)
{
    bool busy = false;
    for (int i = 0; i < B_COUNT; ++i)
    {
        Btn& b = g_btns[i];
        float tgt = (i == g_hoverBtn && b.enabled && b.visible) ? 1.f : 0.f;
        if (EaseTo(b.aHov, tgt, 17.f, dt) > 0) busy = true;
        if (EaseTo(b.aPress, i == g_pressedBtn ? 1.f : 0.f, 30.f, dt) > 0) busy = true;
    }
    for (auto& c : g_chips)
        if (EaseTo(c.aHov, c.hot ? 1.f : 0.f, 17.f, dt) > 0) busy = true;

    if (EaseTo(g_rowHov, g_hoverRow >= 0 ? 1.f : 0.f, 22.f, dt) > 0) busy = true;

    double sd = g_scrollTarget - g_scroll;
    if (fabs(sd) > 0.35) { g_scroll += (float)(sd * MinF(1.f, 15.f * dt)); busy = true; }
    else g_scroll = g_scrollTarget;

    if (g_reveal < 1.f) { g_reveal = ClampF(g_reveal + dt * 3.2f, 0.f, 1.f); busy = true; }

    for (auto& r : g_rows)
        if (r.kind == Row::File && EaseTo(r.aCheck, r.checked ? 1.f : 0.f, 24.f, dt) > 0) { busy = true; break; }

    if (g_scanning) { g_marquee += (int)(dt * 620); busy = true; }
    g_time += dt;
    return busy;
}
static void EnsureTimer()
{
    if (!g_timerOn && g_hwnd) { SetTimer(g_hwnd, 1, 15, nullptr); g_timerOn = true; }
}

// ═══════════════════════════════════════════════════════════════════
//  Действия
// ═══════════════════════════════════════════════════════════════════
static void StartScan(HWND hwnd)
{
    if (g_scanning) return;
    std::wstring raw = GetEditPath();
    std::vector<std::wstring> roots = ParseRoots(raw);
    if (roots.empty())
    {
        MessageBeep(MB_ICONWARNING);
        SetStatus(raw.empty() ? L"Укажите папку — можно несколько через |." : L"Ни одной из указанных папок не найдено.", IC_WARN);
        MessageBoxW(hwnd,
            L"Укажите существующую папку, например C:\\Users\\ВашеИмя\\Downloads.\n\n"
            L"Несколько папок за пуск разделяются вертикальной чертой:\n"
            L"    D:\\Фото|D:\\Архив|E:\\Дистрибутивы",
            L"Mebo Duplicate Cleaner", MB_OK | MB_ICONINFORMATION);
        Layout(); InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    RefreshExtSet(); RefreshDateWindow();
    g_rootsSeen = roots;
    g_scanning = true; g_cancel = false; g_phase = 0;
    g_progDone = 0; g_progTotal = 0; g_filesSeen = 0;
    g_seenFiles = 0; g_skippedFiles = 0;
    g_scanStart = GetTickCount64();
    g_allGroups.clear();
    g_groups.clear();
    BuildRows();
    g_rows.clear();
    g_reveal = 1.f;
    g_scroll = g_scrollTarget = 0;
    g_hint.clear();
    std::wstring f = FiltersSummary();
    SetStatus(roots.size() > 1
                ? L"Перебор файлов в " + std::to_wstring(roots.size()) + L" папках…"
                : L"Перебор файлов…", IC_SPINNER);
    if (!f.empty()) g_status += L"  ·  " + f;
    Layout(); InvalidateRect(hwnd, nullptr, FALSE); EnsureTimer();
    EnableWindow(g_edit, FALSE);

    ScanCtx* ctx = new ScanCtx{ hwnd, roots };
    CloseHandle(CreateThread(nullptr, 0, ScanThread, ctx, 0, nullptr));
}
static void FinishScan(HWND hwnd, std::vector<DupGroup>* groups)
{
    g_scanning = false;
    EnableWindow(g_edit, TRUE);
    if (groups)
    {
        g_allGroups = *groups;
        delete groups;
        RebuildFromRaw();
        std::wstring s = ResultStatus();
        std::wstring fs = FiltersSummary();
        if (!fs.empty() && !g_allGroups.empty()) s += L"  ·  " + fs;
        if (g_skippedFiles > 0 && FiltersActive())
            s += L" · отсеяно " + FmtNum(g_skippedFiles) + L" " +
                 PluralRu(g_skippedFiles, L"файл", L"файла", L"файлов");
        SetStatus(s, (g_allGroups.empty() || !g_groups.empty()) ? IC_OK : IC_WARN);
    }
    else SetStatus(L"Сканирование отменено.", IC_WARN);
    Layout(); InvalidateRect(hwnd, nullptr, FALSE); EnsureTimer();
}
static void DoBrowse(HWND hwnd)
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = g_edit && GetWindowTextLengthW(g_edit) > 0
                 ? L"Добавить папку к списку (через |)" : L"Выберите папку для сканирования";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl)
    {
        wchar_t path[MAX_PATH * 2];
        if (SHGetPathFromIDListW(pidl, path))
        {
            std::wstring cur = GetEditPath();
            // вторая папка добавляется к первой, а не заменяет её
            std::wstring next = cur.empty() ? std::wstring(path) : cur + L"|" + path;
            SetWindowTextW(g_edit, next.c_str());
            g_statusIcon = IC_INFO;
        }
        CoTaskMemFree(pidl);
        Layout(); InvalidateRect(hwnd, nullptr, FALSE);
    }
}
static void CheckAll(bool dupOnly)
{
    for (auto& r : g_rows)
        if (r.kind == Row::File) r.checked = dupOnly ? (r.indexInGroup > 0) : false;
    RecountChecked(); Layout(); EnsureTimer(); InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void ToggleRow(int idx)
{
    if (idx < 0 || idx >= (int)g_rows.size()) return;
    Row& r = g_rows[idx];
    if (r.kind != Row::File || r.indexInGroup == 0) return;
    r.checked = !r.checked;
    RecountChecked(); Layout(); EnsureTimer(); InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void ToggleGroup(int gi)
{
    bool anyUnchecked = false;
    for (auto& r : g_rows)
        if (r.kind == Row::File && r.group == gi && r.indexInGroup > 0 && !r.checked) { anyUnchecked = true; break; }
    for (auto& r : g_rows)
        if (r.kind == Row::File && r.group == gi && r.indexInGroup > 0) r.checked = anyUnchecked;
    RecountChecked(); Layout(); EnsureTimer(); InvalidateRect(g_hwnd, nullptr, FALSE);
}
// ручная замена оригинала: переопределяет правило до следующего сканирования
static void MakeOriginal(int rowIdx)
{
    if (rowIdx < 0 || rowIdx >= (int)g_rows.size()) return;
    Row& r = g_rows[rowIdx];
    if (r.kind != Row::File || r.indexInGroup == 0 || r.group < 0) return;
    std::wstring hash = g_groups[r.group].hash;
    std::wstring target = r.path;
    for (auto& g : g_allGroups)
        if (g.hash == hash) { g.manualOrig = target; break; }
    RebuildFromRaw();
    SetStatus(L"Оригинал выбран вручную — остальные копии помечены к удалению.", IC_OK);
    EnsureTimer(); InvalidateRect(g_hwnd, nullptr, FALSE);
}
// вычёркивание удалённых файлов из сырого результата
static void PurgeMissing()
{
    std::vector<DupGroup> kept;
    for (auto& g : g_allGroups)
    {
        DupGroup ng; ng.size = g.size; ng.hash = g.hash; ng.manualOrig = g.manualOrig;
        for (auto& f : g.files)
            if (GetFileAttributesW(f.path.c_str()) != INVALID_FILE_ATTRIBUTES) ng.files.push_back(f);
        if (ng.files.size() > 1) kept.push_back(ng);
    }
    g_allGroups = kept;
}
static void OnDeleteChecked(HWND hwnd)
{
    std::vector<std::wstring> files;
    for (auto& r : g_rows) if (r.kind == Row::File && r.checked) files.push_back(r.path);
    if (files.empty())
    {
        MessageBeep(MB_ICONINFORMATION);
        SetStatus(L"Нечего удалять — отметьте копии.", IC_WARN);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    std::wstring msg = L"Удалить в корзину файлов: " + std::to_wstring(files.size())
                     + L"\nОсвободится: " + FmtBytes(g_checkedBytes)
                     + L"\n\nФайлы можно будет восстановить из корзины Windows.";
    if (MessageBoxW(hwnd, msg.c_str(), L"Подтверждение удаления", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    SetStatus(L"Удаление в корзину…", IC_TRASH);
    InvalidateRect(hwnd, nullptr, FALSE);

    long long freed = g_checkedBytes;
    int okCnt = 0;
    for (auto& f : files) if (RecycleFile(f)) ++okCnt;

    PurgeMissing();
    RebuildFromRaw();
    if (g_groups.empty()) SetStatus(L"Готово — дубликатов больше нет.", IC_OK);
    else SetStatus(L"Удалено в корзину: " + std::to_wstring(okCnt) + L" · освобождено " + FmtBytes(freed), IC_OK);
    Layout(); InvalidateRect(hwnd, nullptr, FALSE);
}
static void ShowHint(const std::wstring& h) { if (g_hint != h) { g_hint = h; InvalidateRect(g_hwnd, nullptr, FALSE); } }

// ═══════════════════════════════════════════════════════════════
//  Меню фильтров и экспорт  (v0.5.0)
// ═══════════════════════════════════════════════════════════════
static void DoExport(HWND hwnd, bool csv);
static void CopyListToClipboard(HWND hwnd);
static void ShowAbout(HWND hwnd);
// id пунктов: 620+MS, 630+TY, 640+DF, 650+OR, 600 служебные, 610 экспорт
enum
{
    MI_SKIP_SVC = 600, MI_RESET = 601,
    MI_EXPORT_CSV = 610, MI_EXPORT_TXT = 611, MI_COPY_LIST = 612, MI_ABOUT = 613,
    MI_MS = 620, MI_TY = 630, MI_DF = 640, MI_OR = 650
};
static HMENU RadioSub(int* cur, const wchar_t* labels[], int n, int idBase)
{
    HMENU s = CreatePopupMenu();
    for (int i = 0; i < n; ++i)
        AppendMenuW(s, MF_STRING | (*cur == i ? MF_CHECKED : 0), idBase + i, labels[i]);
    return s;
}
static int MenuPick(int* cur, int n, int cmd, int idBase)
{
    if (cmd < idBase || cmd >= idBase + n) return 0;
    int v = cmd - idBase;
    if (v < 0 || v >= n || v == *cur) return 0;
    *cur = v;
    return 1;
}
static void ShowFilterMenu(HWND hwnd)
{
    HMENU root = CreatePopupMenu();
    AppendMenuW(root, MF_POPUP, (UINT_PTR)RadioSub(&g_ms, MS_LABEL, MS_N, MI_MS), L"Минимальный размер");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)RadioSub(&g_ty, TY_LABEL, TY_N, MI_TY), L"Тип файлов");
    AppendMenuW(root, MF_POPUP, (UINT_PTR)RadioSub(&g_df, DF_LABEL, DF_N, MI_DF), L"Дата изменения");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(root, MF_POPUP, (UINT_PTR)RadioSub(&g_or, OR_LABEL, OR_N, MI_OR), L"Что считать оригиналом");
    AppendMenuW(root, MF_STRING | (g_skipSvc ? MF_CHECKED : 0), MI_SKIP_SVC,
                L"Пропускать служебные и скрытые папки");
    AppendMenuW(root, MF_STRING, MI_RESET, L"Сбросить фильтры и правила");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(root, MF_STRING | (g_rows.empty() ? MF_GRAYED : 0), MI_EXPORT_CSV, L"Экспорт в CSV…        Ctrl+S");
    AppendMenuW(root, MF_STRING | (g_rows.empty() ? MF_GRAYED : 0), MI_EXPORT_TXT, L"Экспорт в TXT…");
    AppendMenuW(root, MF_STRING | (g_rows.empty() ? MF_GRAYED : 0), MI_COPY_LIST, L"Скопировать отчёт в буфер");
    AppendMenuW(root, MF_STRING, MI_ABOUT, L"О программе");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenuEx(root, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hwnd, nullptr);
    DestroyMenu(root);
    PostMessageW(hwnd, WM_NULL, 0, 0);          // по-человечески снимаем модальность меню
    if (!cmd) return;

    int changed = 0;
    changed |= MenuPick(&g_ms, MS_N, cmd, MI_MS);
    changed |= MenuPick(&g_ty, TY_N, cmd, MI_TY);
    changed |= MenuPick(&g_df, DF_N, cmd, MI_DF);
    if (MenuPick(&g_or, OR_N, cmd, MI_OR))
        for (auto& g : g_allGroups) g.manualOrig.clear();   // правило поменялось — ручные выборки сбрасываем

    if (cmd == MI_SKIP_SVC) { g_skipSvc = !g_skipSvc; changed = 1; }
    if (cmd == MI_RESET)
    {
        g_ms = MS_OFF; g_ty = TY_ALL; g_df = DF_ANY; g_or = OR_OLD; g_skipSvc = true;
        for (auto& g : g_allGroups) g.manualOrig.clear();
        changed = 1;
    }
    if (changed)
    {
        std::wstring fs = FiltersSummary();
        if (!g_scanning && g_rows.empty())
            SetStatus(fs.empty() ? L"Фильтры сброшены — сканируйте." : L"Фильтры: " + fs + L". Запускайте поиск.", IC_INFO);
        OnFilterChanged();
        if (cmd == MI_SKIP_SVC && !g_allGroups.empty())
            SetStatus(g_skipSvc ? L"Служебные папки снова пропускаются — они исчезнут из списка только после нового поиска."
                                : L"Служебные папки включены в обход — их увидит только новый поиск.", IC_WARN);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    switch (cmd)
    {
        case MI_EXPORT_CSV: DoExport(hwnd, true);  break;
        case MI_EXPORT_TXT: DoExport(hwnd, false); break;
        case MI_COPY_LIST:  CopyListToClipboard(hwnd); break;
        case MI_ABOUT:      ShowAbout(hwnd);       break;
        default: break;
    }
}

// ── запись отчёта ─────────────────────────────────────────────────
static void ShowAbout(HWND hwnd)
{
    std::wstring txt =
        L"Mebo Duplicate Cleaner " + std::wstring(APP_VERSION) + L"\n\n"
        L"Ищет файлы-дубликаты по совпадению SHA-256 и отправляет копии в корзину.\n"
        L"Фильтры по размеру, типу и дате · несколько папок через | · правила выбора\n"
        L"оригинала · экспорт в CSV и TXT.\n\n"
        L"Чистый C++ / WinAPI + GDI+, работает без установки, без сети и телеметрии.\n\n"
        L"© Mebo System";
    MessageBoxW(hwnd, txt.c_str(), L"О программе", MB_OK | MB_ICONINFORMATION);
}
static bool WriteUtf8File(const std::wstring& path, const std::wstring& text)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string body;
    body.resize((size_t)(need > 0 ? need : 0));
    if (need > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &body[0], need, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
    DWORD wr = 0;
    BOOL ok = WriteFile(h, bom, 3, &wr, nullptr);
    if (ok && !body.empty()) ok = WriteFile(h, &body[0], (DWORD)body.size(), &wr, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}
static std::wstring DefaultExportName(bool csv)
{
    return L"mebo-duplicates_" + StampNow() + (csv ? L".csv" : L".txt");
}
static std::wstring SuggestDir()
{
    std::wstring d = KnownFolder(FOLDERID_Documents);
    if (!d.empty()) return d;
    return g_rootsSeen.empty() ? L"" : DirName(g_rootsSeen[0]);
}
static void DoExport(HWND hwnd, bool csv)
{
    if (g_rows.empty())
    {
        MessageBeep(MB_ICONINFORMATION);
        SetStatus(L"Экспортировать нечего — сначала найдите дубликаты.", IC_WARN);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    wchar_t file[MAX_PATH * 4] = L"";
    wcscpy_s(file, DefaultExportName(csv).c_str());
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = hwnd;
    of.lpstrFilter = csv ? L"CSV (разделитель «;», Excel)\0*.csv\0Любые файлы\0*.*\0"
                         : L"Текстовый отчёт\0*.txt\0Любые файлы\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    of.lpstrInitialDir = SuggestDir().c_str();
    of.lpstrDefExt = csv ? L"csv" : L"txt";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&of)) return;
    std::wstring path(file);                       // выбранная пользователем полное имя

    std::wstring text = BuildExport(csv);
    if (!WriteUtf8File(path, text))
    {
        MessageBeep(MB_ICONERROR);
        SetStatus(L"Не удалось записать файл — проверьте, открыт ли он в Excel.", IC_WARN);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    std::wstring dirs = DirName(path);
    SetStatus(L"Отчёт записан: " + BaseName(path), IC_OK);
    Layout(); InvalidateRect(hwnd, nullptr, FALSE);
    std::wstring params = L"/select,\"" + path + L"\"";
    if ((INT_PTR)ShellExecuteW(hwnd, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(hwnd, L"explore", dirs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
static void CopyListToClipboard(HWND hwnd)
{
    std::wstring text = BuildExport(false);
    if (!OpenClipboard(hwnd)) { MessageBeep(MB_ICONERROR); return; }
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg)
    {
        wchar_t* d = (wchar_t*)GlobalLock(hg);
        if (d) { wcscpy_s(d, text.size() + 1, text.c_str()); GlobalUnlock(hg); SetClipboardData(CF_UNICODETEXT, hg); }
        else GlobalFree(hg);
    }
    CloseClipboard();
    SetStatus(L"Отчёт скопирован в буфер обмена.", IC_INFO);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ═══════════════════════════════════════════════════════════════════
//  Взаимодействие мышью
// ═══════════════════════════════════════════════════════════════════
static int RowAt(int clientY)
{
    if (g_rows.empty()) return -1;
    int y = g_rList.top + ROW_TOP - (int)g_scroll;
    for (int i = 0; i < (int)g_rows.size(); ++i)
    {
        int h = RowH(g_rows[i]);
        if (y > g_rList.bottom) break;
        if (clientY >= y && clientY < y + h) return i;
        y += h;
    }
    return -1;
}
static RECT ThumbRect()
{
    int listH = g_rList.bottom - g_rList.top;
    int total = g_scrollMax + listH;
    int thumbH = MaxI(listH * listH / total, S(36));
    int trackY = g_rList.top + S(4);
    int trackH = listH - S(8);
    int thumbY = trackY + (int)((trackH - thumbH) * (g_scroll / (double)(g_scrollMax > 0 ? g_scrollMax : 1)));
    RECT th = { g_wndW - S(14), thumbY, g_wndW - S(1), MinI(trackY + trackH, thumbY + thumbH) };
    return th;
}
static bool ScrollThumbHit(POINT pt)
{
    if (g_scrollMax <= 0) return false;
    RECT th = ThumbRect();
    return PtInRect(&th, pt) != FALSE;
}
static void ScrollBy(int delta)
{
    g_scrollTarget = (float)ClampD(g_scrollTarget + delta, 0, g_scrollMax);
    EnsureTimer();
}

static void OnMouseMove(HWND hwnd, int x, int y)
{
    if (!g_tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t); g_tracking = true; }

    if (g_thumbDrag)
    {
        int listH = g_rList.bottom - g_rList.top;
        int trackH = listH - S(8);
        int total = g_scrollMax + listH;
        int thumbH = MaxI(listH * listH / total, S(36));
        int travel = trackH - thumbH;
        if (travel > 0)
        {
            double f = ClampD((double)(y - g_thumbDragOff - (g_rList.top + S(4))) / travel, 0, 1);
            g_scroll = g_scrollTarget = (float)(f * g_scrollMax);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }

    int hb = -1;
    std::wstring hint;
    for (int i = 0; i < B_COUNT; ++i)
    {
        Btn& b = g_btns[i];
        if (b.visible && ValidRc(b.rc) && PtInRect(&b.rc, POINT{ x, y }))
        {
            if (b.enabled) hb = i;
            hint = b.hint;
            break;
        }
    }
    bool chipHot = false;
    if (ValidRc(g_rChips))
        for (auto& c : g_chips)
        {
            bool hot = ValidRc(c.rc) && PtInRect(&c.rc, POINT{ x, y }) != FALSE;
            c.hot = hot;
            if (hot) { chipHot = true; hb = -1; hint = L"Открыть и просканировать: " + c.path; }
        }

    bool cl = g_edit && GetWindowTextLengthW(g_edit) > 0 && ValidRc(g_rClear) && PtInRect(&g_rClear, POINT{ x, y }) != FALSE;
    if (cl != g_clearHover) { g_clearHover = cl; InvalidateRect(hwnd, nullptr, FALSE); }
    if (cl) hint = L"Очистить поле пути";

    int hr = -1;
    if (y >= g_rList.top && y < g_rList.bottom && x < g_wndW - S(14))
    {
        int r = RowAt(y);
        if (r >= 0 && g_rows[r].kind != Row::Spacer)
        {
            hr = r;
            if (g_rows[r].kind == Row::Group) hint = L"Клик — отметить всю группу · двойной клик — открыть в проводнике";
            else if (g_rows[r].indexInGroup == 0) hint = L"Оригинал останется на диске · двойной клик — открыть в проводнике";
            else hint = L"Клик — отметить к удалению · двойной клик — открыть в проводнике";
        }
    }
    if (hr != g_hoverRow)
    {
        g_prevHoverRow = g_hoverRow;
        g_hoverRow = hr;
        if (hr < 0) g_prevHoverRow = -1;
        g_rowHov = g_prevHoverRow >= 0 && hr >= 0 ? g_rowHov : 0.f;
        EnsureTimer();
    }

    ShowHint(hint);

    bool th = ScrollThumbHit(POINT{ x, y });
    if (th != g_thumbHot) { g_thumbHot = th; InvalidateRect(hwnd, nullptr, FALSE); }

    bool hand = hb >= 0 || hr >= 0 || th || cl || chipHot;
    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
}
static void OnMouseLeave(HWND hwnd)
{
    g_tracking = false; g_hoverBtn = -1;
    g_prevHoverRow = g_hoverRow; g_hoverRow = -1; g_rowHov = 0.f;
    for (auto& c : g_chips) c.hot = false;
    g_clearHover = false; g_thumbHot = false;
    ShowHint(L"");
    EnsureTimer();
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void OnLButtonDown(HWND hwnd, int x, int y)
{
    for (int i = 0; i < B_COUNT; ++i)
    {
        Btn& b = g_btns[i];
        if (b.visible && ValidRc(b.rc) && PtInRect(&b.rc, POINT{ x, y }))
        {
            if (b.enabled) { g_pressedBtn = i; InvalidateRect(hwnd, nullptr, FALSE); }
            return;
        }
    }
    if (ScrollThumbHit(POINT{ x, y }))
    {
        g_thumbDrag = true; SetCapture(hwnd);
        RECT th = ThumbRect();
        g_thumbDragOff = y - th.top;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (ValidRc(g_rChips))
        for (auto& c : g_chips)
            if (ValidRc(c.rc) && PtInRect(&c.rc, POINT{ x, y }))
            {
                SetWindowTextW(g_edit, c.path.c_str());
                StartScan(hwnd);
                return;
            }
    if (y >= g_rList.top && y < g_rList.bottom)
    {
        int r = RowAt(y);
        if (r >= 0)
        {
            if (g_rows[r].kind == Row::Group) ToggleGroup(g_rows[r].group);
            else ToggleRow(r);
            return;
        }
    }
}
static void OnLButtonUp(HWND hwnd, int x, int y)
{
    if (g_thumbDrag) { g_thumbDrag = false; ReleaseCapture(); InvalidateRect(hwnd, nullptr, FALSE); return; }
    if (g_pressedBtn < 0) return;
    int i = g_pressedBtn; g_pressedBtn = -1;
    bool inside = ValidRc(g_btns[i].rc) && PtInRect(&g_btns[i].rc, POINT{ x, y }) && g_btns[i].enabled;
    InvalidateRect(hwnd, nullptr, FALSE);
    if (!inside) return;
    switch (i)
    {
        case B_BROWSE:  DoBrowse(hwnd); break;
        case B_FILTERS: ShowFilterMenu(hwnd); break;
        case B_SCAN:    StartScan(hwnd); break;
        case B_CANCEL:  g_cancel = true; SetStatus(L"Отменяю…", IC_WARN); InvalidateRect(hwnd, nullptr, FALSE); break;
        case B_UNCHECK: CheckAll(false); break;
        case B_AUTO:    CheckAll(true); break;
        case B_DELETE:  OnDeleteChecked(hwnd); break;
    }
}
static void OnDblClick(HWND hwnd, int x, int y)
{
    (void)hwnd; (void)x;
    if (y < g_rList.top || y >= g_rList.bottom) return;
    int r = RowAt(y);
    if (r < 0) return;
    if (g_rows[r].kind == Row::Group)
    {
        int gi = g_rows[r].group;
        ToggleGroup(gi);                       // откат одиночного клика
        std::wstring target;
        for (auto& rr : g_rows)
            if (rr.kind == Row::File && rr.group == gi) { target = rr.path; break; }
        if (!target.empty()) OpenInExplorer(target);
        return;
    }
    ToggleRow(r);                              // откат клика
    if (!g_rows[r].path.empty()) OpenInExplorer(g_rows[r].path);
}
static void OnWheel(HWND hwnd, int delta)
{
    int step = (delta / 120) * (ROW_FILE * 3);
    ScrollBy(-step);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void CopyToClipboard(const std::wstring& s)
{
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg)
    {
        wchar_t* d = (wchar_t*)GlobalLock(hg);
        if (d) { wcscpy_s(d, s.size() + 1, s.c_str()); GlobalUnlock(hg); SetClipboardData(CF_UNICODETEXT, hg); }
    }
    CloseClipboard();
}
static void ShowRowMenu(HWND hwnd, int row, POINT screenPt)
{
    if (row < 0 || row >= (int)g_rows.size() || g_rows[row].kind != Row::File) return;
    Row& r = g_rows[row];
    bool isOrig = r.indexInGroup == 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Показать в проводнике");
    AppendMenuW(m, MF_STRING, 2, L"Открыть файл");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Скопировать полный путь");
    AppendMenuW(m, MF_STRING | (isOrig ? MF_GRAYED : 0), 4, r.checked ? L"Снять отметку" : L"Отметить к удалению");
    AppendMenuW(m, MF_STRING | (isOrig ? MF_GRAYED : 0), 6, L"Сделать оригиналом группы");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 5, L"Удалить этот файл в корзину…");
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd)
    {
        case 1: OpenInExplorer(r.path); break;
        case 2: ShellExecuteW(hwnd, L"open", r.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case 3: CopyToClipboard(r.path); SetStatus(L"Путь скопирован в буфер обмена.", IC_INFO); InvalidateRect(hwnd, nullptr, FALSE); break;
        case 4: ToggleRow(row); break;
        case 6: MakeOriginal(row); break;
        case 5:
        {
            std::wstring q = L"Удалить в корзину:\n" + r.path;
            if (MessageBoxW(hwnd, q.c_str(), L"Подтверждение", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            if (RecycleFile(r.path)) SetStatus(L"Файл удалён в корзину.", IC_OK);
            else SetStatus(L"Файл не удалён (возможно, занят).", IC_WARN);
            PurgeMissing();
            RebuildFromRaw();
            if (g_groups.empty()) SetStatus(L"Дубликатов больше нет.", IC_OK);
            Layout(); InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
    }
}
static bool HandleHotkey(HWND hwnd, int vk)
{
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_F5 || (ctrl && vk == 'E')) { if (!g_scanning) StartScan(hwnd); return true; }
    if (vk == VK_ESCAPE)
    {
        if (g_scanning) { g_cancel = true; SetStatus(L"Отменяю…", IC_WARN); InvalidateRect(hwnd, nullptr, FALSE); }
        else DestroyWindow(hwnd);
        return true;
    }
    if (ctrl && vk == 'A') { if (!g_scanning) CheckAll(true); return true; }
    if (ctrl && vk == 'D') { CheckAll(false); return true; }
    if (ctrl && vk == 'B') { if (!g_scanning) DoBrowse(hwnd); return true; }
    if (ctrl && vk == 'F') { if (!g_scanning) ShowFilterMenu(hwnd); return true; }
    if (ctrl && vk == 'S') { if (!g_scanning) DoExport(hwnd, shift ? false : true); return true; }
    if (vk == DELETE) { if (!g_scanning && !g_rows.empty()) OnDeleteChecked(hwnd); return true; }
    return false;
}

// ── подкласс поля ввода ────────────────────────────────────────────
static WNDPROC g_oldEditProc = nullptr;
static LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
        case WM_KEYDOWN:
            if (w == VK_RETURN) { if (!g_scanning) StartScan(g_hwnd); return 0; }
            if (w == VK_ESCAPE && GetWindowTextLengthW(h) > 0) { SetWindowTextW(h, L""); InvalidateRect(g_hwnd, nullptr, FALSE); return 0; }
            if (w == VK_ESCAPE || w == VK_F5 || w == DELETE) { if (HandleHotkey(g_hwnd, (int)w)) return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) &&
                (w == 'A' || w == 'D' || w == 'E' || w == 'B' || w == 'F' || w == 'S'))
                if (HandleHotkey(g_hwnd, (int)w)) return 0;
            break;
        case WM_SETCURSOR:
            if (g_clearHover) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_LBUTTONDOWN:
        {
            POINT scr = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ClientToScreen(h, &scr);
            ScreenToClient(g_hwnd, &scr);
            bool over = GetWindowTextLengthW(h) > 0 && ValidRc(g_rClear) && PtInRect(&g_rClear, scr) != FALSE;
            if (m == WM_MOUSEMOVE)
            {
                if (over != g_clearHover) { g_clearHover = over; InvalidateRect(g_hwnd, nullptr, FALSE); }
                break;
            }
            if (over) { if (m == WM_LBUTTONUP) { SetWindowTextW(h, L""); InvalidateRect(g_hwnd, nullptr, FALSE); } return 0; }
            break;
        }
        case WM_CONTEXTMENU:
        {
            HMENU mm = CreatePopupMenu();
            AppendMenuW(mm, MF_STRING, 1, L"Вставить из буфера обмена");
            AppendMenuW(mm, MF_STRING, 2, L"Очистить");
            int cmd = TrackPopupMenuEx(mm, TPM_RETURNCMD | TPM_RIGHTBUTTON, GET_X_LPARAM(l), GET_Y_LPARAM(l), h, nullptr);
            DestroyMenu(mm);
            if (cmd == 1) SendMessageW(h, WM_PASTE, 0, 0);
            if (cmd == 2) { SetWindowTextW(h, L""); InvalidateRect(g_hwnd, nullptr, FALSE); }
            return 0;
        }
    }
    return CallWindowProcW(g_oldEditProc, h, m, w, l);
}

// ═══════════════════════════════════════════════════════════════════
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_PAINT: OnPaint(hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:   OnMouseMove(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSELEAVE:  OnMouseLeave(hwnd); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP:   OnLButtonUp(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONDBLCLK: OnDblClick(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        {
            POINT sp = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // экранные координаты
            if (lp == 0 || (short)LOWORD(lp) == -1) GetCursorPos(&sp);
            POINT pt = sp; ScreenToClient(hwnd, &pt);
            int r = RowAt(pt.y);
            if (r >= 0 && g_rows[r].kind == Row::File && pt.y >= g_rList.top && pt.y < g_rList.bottom)
            {
                if (g_hoverRow != r) { g_prevHoverRow = g_hoverRow; g_hoverRow = r; g_rowHov = 0.f; EnsureTimer(); }
                ShowRowMenu(hwnd, r, sp);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | (g_scanning ? MF_GRAYED : 0), 101, L"Сканировать              F5");
            AppendMenuW(m, MF_STRING | (g_scanning ? MF_GRAYED : 0), 102, L"Добавить папку…           Ctrl+B");
            AppendMenuW(m, MF_STRING | (g_scanning ? MF_GRAYED : 0), 104, L"Фильтры и правила…       Ctrl+F");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (g_rows.empty() ? MF_GRAYED : 0), 105, L"Экспорт в CSV…          Ctrl+S");
            AppendMenuW(m, MF_STRING | (g_rows.empty() ? MF_GRAYED : 0), 106, L"Экспорт в TXT…          Ctrl+Shift+S");
            AppendMenuW(m, MF_STRING | (g_rows.empty() ? MF_GRAYED : 0), 107, L"Скопировать отчёт в буфер");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 103, L"О программе");
            int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 101 && !g_scanning) StartScan(hwnd);
            if (cmd == 102 && !g_scanning) DoBrowse(hwnd);
            if (cmd == 104 && !g_scanning) ShowFilterMenu(hwnd);
            if (cmd == 105) DoExport(hwnd, true);
            if (cmd == 106) DoExport(hwnd, false);
            if (cmd == 107) CopyListToClipboard(hwnd);
            if (cmd == 103) ShowAbout(hwnd);
            return 0;
        }
        case WM_MOUSEWHEEL: OnWheel(hwnd, GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_KEYDOWN:    if (HandleHotkey(hwnd, (int)wp)) return 0; break;
        case WM_TIMER:
        {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            float dt = (float)((double)(now.QuadPart - g_perfPrev.QuadPart) / (double)g_perfFreq.QuadPart);
            g_perfPrev = now;
            if (dt > 0.2f) dt = 0.2f;
            if (dt < 0) dt = 0;
            bool busy = TickAnim(dt);
            if (busy) InvalidateRect(hwnd, nullptr, FALSE);
            else if (g_timerOn) { KillTimer(hwnd, 1); g_timerOn = false; }
            return 0;
        }
        case WM_APP_PROGRESS:
            if (wp == 0) { g_phase = 0; g_filesSeen = (long long)(INT_PTR)lp; }
            else { g_phase = 1; g_progDone = (long long)(INT_PTR)lp; }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_TOTAL:
            g_phase = 1; g_progTotal = (long long)(INT_PTR)wp; g_filesSeen = (long long)(INT_PTR)lp;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_DONE:
            FinishScan(hwnd, (std::vector<DupGroup>*)wp);
            return 0;
        case WM_APP_AUTOSTART:
            StartScan(hwnd);
            return 0;
        case WM_COMMAND:
            switch (HIWORD(wp))
            {
                case EN_SETFOCUS: g_editFocus = true; InvalidateRect(hwnd, nullptr, FALSE); break;
                case EN_KILLFOCUS: g_editFocus = false; g_clearHover = false; InvalidateRect(hwnd, nullptr, FALSE); break;
                case EN_CHANGE: InvalidateRect(hwnd, nullptr, FALSE); break;
            }
            return 0;
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = S(760);
            mmi->ptMinTrackSize.y = S(540);
            return 0;
        }
        case WM_DPICHANGED:
        {
            RECT* nr = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            g_scale = LOWORD(wp) / 96.0;
            FreeFonts(); InitFonts();
            if (g_edit) SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fMono, TRUE);
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void InitChips()
{
    struct Def { REFKNOWNFOLDERID id; IconId ic; const wchar_t* title; };
    Def defs[] = {
        { FOLDERID_Downloads, IC_DOWNLOAD, L"Загрузки" },
        { FOLDERID_Documents, IC_DOC,      L"Документы" },
        { FOLDERID_Desktop,   IC_MONITOR,  L"Рабочий стол" },
        { FOLDERID_Pictures,  IC_PICTURE,  L"Изображения" },
    };
    g_chips.clear();
    for (auto& d : defs)
    {
        std::wstring p = KnownFolder(d.id);
        if (p.empty() || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        Chip c; c.icon = d.ic; c.title = d.title; c.path = p;
        g_chips.push_back(c);
    }
}

static HICON MakeAppIcon(int sz)
{
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz; bi.bV5Height = -sz;
    bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    memset(bits, 0, (size_t)sz * sz * 4);

    HDC mdc = CreateCompatibleDC(nullptr);
    HBITMAP oldb = (HBITMAP)SelectObject(mdc, color);
    Graphics gx(mdc);
    gx.SetSmoothingMode(SmoothingModeAntiAlias);
    gx.SetPixelOffsetMode(PixelOffsetModeHalf);
    Graphics* keep = g_gx; g_gx = &gx;
    RECT r{ 0, 0, sz, sz };
    float rad = (float)(sz * 0.22);
    FillRound(r, rad, C_ACCENT_HI, C_ACCENT_LO);
    float k = sz / 24.f;
    RECT b1{ (int)(9 * k), (int)(4 * k), (int)(20 * k), (int)(14 * k) };
    FillSolid(b1, 3 * k, RGB(255, 255, 255), 0.45);
    RECT b2{ (int)(4 * k), (int)(9 * k), (int)(15 * k), (int)(20 * k) };
    FillSolid(b2, 3 * k, RGB(255, 255, 255), 0.95);
    PointF ch[] = { { 6.5f * k, 14.5f * k }, { 8.6f * k, 16.6f * k }, { 12.2f * k, 11.4f * k } };
    PolyA(ch, 3, C_ACCENT_LO, 0.9, (float)MaxI(1, sz / 14));
    g_gx = keep;
    gx.Flush();
    SelectObject(mdc, oldb);
    DeleteDC(mdc);

    unsigned char* px = (unsigned char*)bits;
    for (int i = 0; i < sz * sz; ++i)
    {
        unsigned char a = px[i * 4 + 3];
        px[i * 4 + 0] = (unsigned char)(px[i * 4 + 0] * a / 255);
        px[i * 4 + 1] = (unsigned char)(px[i * 4 + 1] * a / 255);
        px[i * 4 + 2] = (unsigned char)(px[i * 4 + 2] * a / 255);
    }
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = (BOOL(WINAPI*)(HANDLE))GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4);                       // PER_MONITOR_AWARE_V2
    else { auto sd = (BOOL(WINAPI*)())GetProcAddress(u32, "SetProcessDPIAware"); if (sd) sd(); }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR gpTok = 0;
    GdiplusStartupInput gpsi;
    GdiplusStartup(&gpTok, &gpsi, nullptr);

    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_perfPrev);

    HDC screen = GetDC(nullptr);
    g_scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, screen);
    ComputeSizes();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MeboDupCleaner";
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    int W = S(1080), H = S(740);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    RECT wr = { 0, 0, W, H };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    W = wr.right - wr.left; H = wr.bottom - wr.top;
    int px = (int)ClampD((sw - W) / 2.0, 0, sw - W);
    int py = (int)ClampD((sh - H) / 2.0 - S(20), 0, sh - H);

    g_hwnd = CreateWindowExW(0, L"MeboDupCleaner",
        L"Mebo Duplicate Cleaner — Mebo System",
        WS_OVERLAPPEDWINDOW, px, py, W, H, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    InitFonts();
    g_brInput = CreateSolidBrush(C_INPUT);
    InitChips();

    HICON big = MakeAppIcon(32), small = MakeAppIcon(16);
    if (big)   SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);

    g_edit = CreateWindowExW(0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
        0, 0, 10, 10, g_hwnd, (HMENU)1, hInst, nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fMono, TRUE);
    g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditSub);

    // папки из командной строки: несколько аргументов и «|» внутри аргумента
    std::wstring startArg;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a.empty() || a[0] == L'-' || a[0] == L'/') continue;
        for (auto& p : ParseRoots(a)) { if (!startArg.empty()) startArg += L'|'; startArg += p; }
    }
    if (argv) LocalFree(argv);
    bool autoStart = !startArg.empty();
    if (autoStart) SetWindowTextW(g_edit, startArg.c_str());

    Layout();

    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof(dark));
    int pref = 2;                                        // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, 33, &pref, sizeof(pref));
    COLORREF caption = RGB(28, 34, 43), bd = RGB(48, 54, 61);
    DwmSetWindowAttribute(g_hwnd, 35, &caption, sizeof(caption));   // DWMWA_CAPTION_COLOR
    DwmSetWindowAttribute(g_hwnd, 34, &bd, sizeof(bd));             // DWMWA_BORDER_COLOR

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    EnsureTimer();
    if (autoStart) PostMessageW(g_hwnd, WM_APP_AUTOSTART, 0, 0);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(g_hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_oldEditProc && g_edit) SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)g_oldEditProc);
    if (big) DestroyIcon(big);
    if (small) DestroyIcon(small);
    FreeFonts();
    if (g_brInput) DeleteObject(g_brInput);
    if (g_mdc) DeleteDC(g_mdc);
    GdiplusShutdown(gpTok);
    CoUninitialize();
    return (int)m.wParam;
}
