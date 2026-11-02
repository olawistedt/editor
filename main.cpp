// Editor - a small dark-mode tabbed text editor with autosave, find and replace.
// Win32 + RichEdit for the text surface, GDI+ for the custom tab strip / find bar / status bar.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <ole2.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

// ---------------------------------------------------------------- constants

enum class Enc { Utf8, Utf8Bom, Utf16, Ansi };
enum class Eol { Crlf, Lf };

constexpr COLORREF cStrip    = RGB(0x18, 0x18, 0x18);  // title / tab strip / status bar
constexpr COLORREF cBg       = RGB(0x20, 0x20, 0x20);  // editor, selected tab, find panel
constexpr COLORREF cTabHover = RGB(0x2A, 0x2A, 0x2A);
constexpr COLORREF cField    = RGB(0x2D, 0x2D, 0x2D);
constexpr COLORREF cBorder   = RGB(0x44, 0x44, 0x44);
constexpr COLORREF cBtnHover = RGB(0x3A, 0x3A, 0x3A);
constexpr COLORREF cSep      = RGB(0x33, 0x33, 0x33);
constexpr COLORREF cText     = RGB(0xE6, 0xE6, 0xE6);
constexpr COLORREF cEditText = RGB(0xDC, 0xDC, 0xDC);
constexpr COLORREF cDim      = RGB(0x9D, 0x9D, 0x9D);
constexpr COLORREF cAccent   = RGB(0x4C, 0xC2, 0xFF);
constexpr COLORREF cWarn     = RGB(0xFF, 0x99, 0xA4);

enum {
    IDM_NEW = 101, IDM_OPEN, IDM_SAVE, IDM_SAVEAS, IDM_CLOSETAB, IDM_EXIT,
    IDM_NEXTTAB, IDM_PREVTAB,
    IDM_FIND, IDM_REPLACE, IDM_FINDNEXT, IDM_FINDPREV, IDM_REPLACEONE, IDM_REPLACEALL, IDM_CLOSEFIND,
    IDM_UNDO, IDM_REDO, IDM_CUT, IDM_COPY, IDM_PASTE, IDM_SELALL, IDM_DATETIME,
    IDM_ZOOMIN, IDM_ZOOMOUT, IDM_ZOOMRESET, IDM_WRAP, IDM_STATUSBAR,
    IDM_MATCHCASE, IDM_WHOLEWORD, IDM_WRAPAROUND, IDM_MENU,
    IDM_TAB1 = 300  // .. 308
};
constexpr int IDC_DOC = 1001, IDC_FIND = 2001, IDC_REPL = 2002;
constexpr UINT_PTR T_ANIM = 1, T_TICK = 2;
constexpr wchar_t kClass[] = L"EditorMainWindow";
constexpr wchar_t kAppName[] = L"Editor";

// ---------------------------------------------------------------- types

struct Doc {
    HWND edit = nullptr;
    std::wstring path;      // empty => untitled (autosaved into the app data folder)
    int untitledId = 0;
    Enc enc = Enc::Utf8;
    Eol eol = Eol::Crlf;
    bool dirty = false;     // changes not yet written to disk
    bool pending = false;   // autosave scheduled
    bool loading = false;
    DWORD lastChange = 0, firstPending = 0;
    FILETIME mtime{};
    // animation state
    float ax = 0, aw = 0, hov = 0, chov = 0;
};

enum BtnKind { K_TEXT, K_TOGGLE, K_UP, K_DOWN, K_CLOSE, K_PLUS, K_MENU };
struct Btn {
    int cmd; BtnKind kind; std::wstring label; bool* flag;
    RectF r{}; bool vis = false; float hov = 0;
};
enum { B_PLUS, B_MENU, B_PREV, B_NEXT, B_CASE, B_WORD, B_WRAP, B_CLOSEFIND, B_REPLACE, B_REPLALL, B_COUNT };

// ---------------------------------------------------------------- globals

HINSTANCE g_inst;
HWND g_hwnd, g_find, g_repl;
int g_dpi = 96;
std::vector<std::unique_ptr<Doc>> g_docs;
int g_cur = -1;

bool g_wrap = true, g_showStatus = true;
bool g_findVis = false, g_replVis = false;
bool g_matchCase = false, g_wholeWord = false, g_wrapAround = true;
int g_zoom = 100;
HWND g_lastFocus = nullptr;

std::vector<Btn> g_btns;
int g_hot = -1, g_pressBtn = -1;
int g_hotTab = -1; bool g_hotClose = false;
Doc* g_pressClose = nullptr;
int g_dragIdx = -1; float g_dragOff = 0; int g_dragStartX = 0; bool g_dragging = false;
bool g_tracking = false;

RectF g_fieldFind, g_fieldRepl, g_stEol;
std::wstring g_findMsg; bool g_findMsgBad = false;
std::wstring g_msg; COLORREF g_msgColor = cDim; DWORD g_msgUntil = 0;
bool g_statusDirty = true;
bool g_startMax = false, g_placed = false;
int g_stLine = 1, g_stCol = 1, g_stSel = 0, g_stChars = 0;

HFONT g_editFont; HBRUSH g_fieldBrush;
std::unique_ptr<Font> g_uiFont, g_smallFont;
std::unique_ptr<StringFormat> g_fLeft, g_fCenter, g_fRight;
HDC g_mdc; HBITMAP g_bmp, g_oldBmp; int g_bw, g_bh;
std::wstring g_dataDir;

int S(int v) { return MulDiv(v, g_dpi, 96); }
float Sf(float v) { return v * g_dpi / 96.0f; }
Color C(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }
COLORREF mix(COLORREF a, COLORREF b, float t) {
    auto m = [&](int x, int y) { return (int)(x + (y - x) * t + 0.5f); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}
int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// forward declarations
void layout();
void setCur(int i);
bool saveDoc(Doc* d);
void saveSession();
void invalidateTop();
void ensureAnim();
void onCommand(int id);
void setZoom(int pct);
bool doFind(bool down, bool fromSelStart);

Doc* curDoc() { return (g_cur >= 0 && g_cur < (int)g_docs.size()) ? g_docs[g_cur].get() : nullptr; }
int indexOf(const Doc* d) {
    for (size_t i = 0; i < g_docs.size(); i++) if (g_docs[i].get() == d) return (int)i;
    return -1;
}
Doc* docByEdit(HWND h) {
    for (auto& d : g_docs) if (d->edit == h) return d.get();
    return nullptr;
}

void toast(const std::wstring& s, COLORREF c = cDim, DWORD ms = 3000) {
    g_msg = s; g_msgColor = c; g_msgUntil = GetTickCount() + ms; g_statusDirty = true;
}

// ---------------------------------------------------------------- file helpers

bool readAll(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    size_t done = 0; bool ok = true;
    while (done < out.size()) {
        DWORD got = 0;
        DWORD chunk = (DWORD)(std::min)(out.size() - done, (size_t)(64u << 20));
        if (!ReadFile(h, &out[done], chunk, &got, nullptr) || got == 0) { ok = false; break; }
        done += got;
    }
    CloseHandle(h);
    out.resize(done);
    return ok;
}

bool writeAll(const std::wstring& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0; bool ok = true;
    while (done < data.size()) {
        DWORD put = 0;
        DWORD chunk = (DWORD)(std::min)(data.size() - done, (size_t)(64u << 20));
        if (!WriteFile(h, data.data() + done, chunk, &put, nullptr) || put == 0) { ok = false; break; }
        done += put;
    }
    CloseHandle(h);
    return ok;
}

FILETIME getMtime(const std::wstring& p) {
    FILETIME ft{};
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) ft = fa.ftLastWriteTime;
    return ft;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Decode raw file bytes. Output text uses '\r' as the only paragraph separator (RichEdit's native form).
std::wstring decodeBytes(const std::string& b, Enc& enc, Eol& eol) {
    std::wstring w;
    auto u = [&](size_t i) { return (unsigned char)b[i]; };
    if (b.size() >= 2 && u(0) == 0xFF && u(1) == 0xFE) {
        enc = Enc::Utf16;
        w.assign((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    } else if (b.size() >= 2 && u(0) == 0xFE && u(1) == 0xFF) {
        enc = Enc::Utf16;
        w.resize((b.size() - 2) / 2);
        for (size_t i = 0; i < w.size(); i++) w[i] = (wchar_t)((u(2 + 2 * i) << 8) | u(3 + 2 * i));
    } else {
        size_t off = 0;
        enc = Enc::Utf8;
        if (b.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF) { enc = Enc::Utf8Bom; off = 3; }
        if (b.size() > off) {
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data() + off, (int)(b.size() - off), nullptr, 0);
            UINT cp = CP_UTF8;
            if (n == 0) {
                enc = Enc::Ansi; cp = CP_ACP; off = 0;
                n = MultiByteToWideChar(cp, 0, b.data(), (int)b.size(), nullptr, 0);
            }
            w.resize(n);
            MultiByteToWideChar(cp, 0, b.data() + off, (int)(b.size() - off), &w[0], n);
        }
    }
    size_t nl = w.find(L'\n');
    eol = (nl == std::wstring::npos || (nl > 0 && w[nl - 1] == L'\r')) ? Eol::Crlf : Eol::Lf;
    std::wstring out;
    out.reserve(w.size());
    for (size_t i = 0; i < w.size(); i++) {
        wchar_t c = w[i];
        if (c == L'\r') { if (i + 1 < w.size() && w[i + 1] == L'\n') i++; out.push_back(L'\r'); }
        else if (c == L'\n') out.push_back(L'\r');
        else out.push_back(c);
    }
    return out;
}

std::string encodeText(const std::wstring& t, Enc enc, Eol eol) {
    std::wstring w;
    w.reserve(t.size() + t.size() / 16);
    for (wchar_t c : t) {
        if (c == L'\r' || c == L'\v') { if (eol == Eol::Crlf) w.push_back(L'\r'); w.push_back(L'\n'); }
        else w.push_back(c);
    }
    std::string s;
    if (enc == Enc::Utf16) {
        s.assign("\xFF\xFE", 2);
        s.append((const char*)w.data(), w.size() * sizeof(wchar_t));
    } else if (enc == Enc::Ansi) {
        if (!w.empty()) {
            int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            s.resize(n);
            WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
        }
    } else {
        if (enc == Enc::Utf8Bom) s.assign("\xEF\xBB\xBF", 3);
        s += toUtf8(w);
    }
    return s;
}

std::wstring backupPath(int id) { return g_dataDir + L"\\untitled-" + std::to_wstring(id) + L".txt"; }

// ---------------------------------------------------------------- rich edit helpers

LONG textLen(HWND e) {
    GETTEXTLENGTHEX gl{ GTL_NUMCHARS, 1200 };
    return (LONG)SendMessageW(e, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
}
std::wstring getText(HWND e) {
    LONG len = textLen(e);
    std::wstring buf(len + 1, L'\0');
    GETTEXTEX gt{};
    gt.cb = (DWORD)((len + 1) * sizeof(wchar_t));
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    LRESULT n = SendMessageW(e, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)buf.data());
    buf.resize((size_t)n);
    return buf;
}
void applyFormat(HWND e) {
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
    cf.crTextColor = cEditText;
    cf.yHeight = 11 * 20;
    wcscpy_s(cf.szFaceName, L"Consolas");
    SendMessageW(e, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}
void setDocText(Doc* d, const std::wstring& w) {
    d->loading = true;
    SETTEXTEX st{ ST_DEFAULT, 1200 };
    SendMessageW(d->edit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)w.c_str());
    applyFormat(d->edit);
    SendMessageW(d->edit, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(d->edit, EM_SETSEL, 0, 0);
    SendMessageW(d->edit, EM_SETMODIFY, FALSE, 0);
    d->loading = false;
    g_statusDirty = true;
}
void applyWrap(Doc* d) { SendMessageW(d->edit, EM_SETTARGETDEVICE, 0, g_wrap ? 0 : 1); }
void applyZoom(Doc* d) {
    if (g_zoom == 100) SendMessageW(d->edit, EM_SETZOOM, 0, 0);
    else SendMessageW(d->edit, EM_SETZOOM, g_zoom, 100);
}

// ---------------------------------------------------------------- subclasses

LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
            setZoom(g_zoom + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 10 : -10));
            return 0;
        }
        break;
    case WM_DROPFILES:
        SendMessageW(g_hwnd, WM_DROPFILES, w, l);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK FindSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            if (h == g_find) onCommand((GetKeyState(VK_SHIFT) & 0x8000) ? IDM_FINDPREV : IDM_FINDNEXT);
            else onCommand(IDM_REPLACEONE);
            return 0;
        }
        if (w == VK_TAB) {
            HWND other = (h == g_find && g_replVis) ? g_repl : g_find;
            SetFocus(other);
            SendMessageW(other, EM_SETSEL, 0, -1);
            return 0;
        }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        break;
    case WM_CHAR:
        if (w == VK_RETURN || w == VK_TAB || w == VK_ESCAPE || w == 1) return 0;
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// ---------------------------------------------------------------- layout

constexpr int kTabH = 40;
int statusH() { return g_showStatus ? S(26) : 0; }
int findH() { return !g_findVis ? 0 : (g_replVis ? S(8 + 32 + 8 + 32 + 8) : S(8 + 32 + 8)); }
RECT clientRc() { RECT rc; GetClientRect(g_hwnd, &rc); return rc; }

RECT editRect() {
    RECT rc = clientRc();
    rc.top = S(kTabH) + findH();
    rc.bottom -= statusH();
    if (rc.bottom < rc.top) rc.bottom = rc.top;
    return rc;
}
void layoutEdit(Doc* d) {
    RECT r = editRect();
    MoveWindow(d->edit, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}

float tabTargetW(int w) {
    int n = (int)g_docs.size();
    if (n == 0) return 0;
    float avail = (float)(w - S(8) - S(8) - S(36) - S(38));
    return std::clamp(avail / n, Sf(60), Sf(240));
}
float tabTargetX(int i, int w) { return Sf(8) + i * tabTargetW(w); }

void layoutStrip(int w) {
    Btn& plus = g_btns[B_PLUS]; Btn& menu = g_btns[B_MENU];
    float menuW = Sf(34), btnH = Sf(30), y = Sf(8);
    menu.r = RectF((float)w - Sf(8) - menuW, y, menuW, btnH);
    float px = Sf(8);
    if (!g_docs.empty()) { Doc* l = g_docs.back().get(); px = l->ax + l->aw + Sf(4); }
    px = (std::min)(px, menu.r.X - Sf(38));
    plus.r = RectF(px, y, Sf(34), btnH);
    plus.vis = menu.vis = true;
}

RectF tabCloseRect(const Doc* d) {
    float cs = Sf(22), cy = Sf(6) + (Sf(kTabH) - Sf(6) - Sf(2)) / 2;
    return RectF(d->ax + d->aw - Sf(6) - cs, cy - cs / 2, cs, cs);
}

void layoutFind() {
    int w = clientRc().right;
    float pad = Sf(16), fh = Sf(32), gap = Sf(8);
    float y1 = Sf(kTabH) + Sf(8), y2 = y1 + fh + Sf(8);
    float fw = Sf(300);
    g_fieldFind = RectF(pad, y1, fw, fh);
    g_fieldRepl = RectF(pad, y2, fw, fh);
    bool fv = g_findVis, rv = g_findVis && g_replVis;

    g_btns[B_PREV].r = RectF(pad + fw + gap, y1, Sf(32), fh);
    g_btns[B_NEXT].r = RectF(pad + fw + gap + Sf(34), y1, Sf(32), fh);
    float x = pad + fw + gap + Sf(34) + Sf(32) + Sf(12);
    float ws[3] = { Sf(96), Sf(94), Sf(106) };
    int ids[3] = { B_CASE, B_WORD, B_WRAP };
    for (int i = 0; i < 3; i++) { g_btns[ids[i]].r = RectF(x, y1, ws[i], fh); x += ws[i] + Sf(6); }
    g_btns[B_CLOSEFIND].r = RectF((float)w - pad - Sf(32), y1, Sf(32), fh);
    g_btns[B_REPLACE].r = RectF(pad + fw + gap, y2, Sf(86), fh);
    g_btns[B_REPLALL].r = RectF(pad + fw + gap + Sf(92), y2, Sf(104), fh);
    for (int i = B_PREV; i <= B_CLOSEFIND; i++) g_btns[i].vis = fv;
    g_btns[B_REPLACE].vis = g_btns[B_REPLALL].vis = rv;

    int eh = S(20);
    auto place = [&](HWND e, const RectF& f, bool show) {
        SetWindowPos(e, nullptr, (int)(f.X + Sf(10)), (int)(f.Y + (f.Height - eh) / 2), (int)(f.Width - Sf(20)), eh,
                     SWP_NOZORDER | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    };
    place(g_find, g_fieldFind, fv);
    place(g_repl, g_fieldRepl, rv);
}

void layout() {
    layoutFind();
    if (Doc* d = curDoc()) layoutEdit(d);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void invalidateTop() {
    RECT r = clientRc();
    r.bottom = S(kTabH) + findH();
    InvalidateRect(g_hwnd, &r, FALSE);
}
void invalidateStatus() {
    RECT r = clientRc();
    r.top = r.bottom - statusH();
    InvalidateRect(g_hwnd, &r, FALSE);
}

// ---------------------------------------------------------------- animation

bool step(float& v, float t, float rate = 0.3f, float eps = 0.02f) {
    float d = t - v;
    if (std::fabs(d) < eps) { if (v != t) { v = t; return true; } return false; }
    v += d * rate;
    return true;
}

void ensureAnim() { SetTimer(g_hwnd, T_ANIM, 15, nullptr); }

void tickAnim() {
    bool any = false;
    int w = clientRc().right;
    float tw = tabTargetW(w);
    for (size_t i = 0; i < g_docs.size(); i++) {
        Doc* d = g_docs[i].get();
        if ((int)i == g_dragIdx && g_dragging) any |= step(d->aw, tw, 0.3f, 0.5f);
        else { any |= step(d->ax, tabTargetX((int)i, w), 0.3f, 0.5f); any |= step(d->aw, tw, 0.3f, 0.5f); }
        any |= step(d->hov, (int)i == g_hotTab ? 1.f : 0.f);
        any |= step(d->chov, ((int)i == g_hotTab && g_hotClose) ? 1.f : 0.f);
    }
    for (size_t i = 0; i < g_btns.size(); i++) any |= step(g_btns[i].hov, (int)i == g_hot ? 1.f : 0.f);
    if (!any) KillTimer(g_hwnd, T_ANIM);
    invalidateTop();
}

// ---------------------------------------------------------------- document management

int nextUntitledId() {
    for (int i = 1;; i++) {
        bool used = false;
        for (auto& d : g_docs) if (d->untitledId == i) { used = true; break; }
        if (!used) return i;
    }
}

std::wstring docTitle(const Doc* d) {
    if (!d->path.empty()) {
        size_t p = d->path.find_last_of(L"\\/");
        return p == std::wstring::npos ? d->path : d->path.substr(p + 1);
    }
    return d->untitledId > 1 ? L"Untitled " + std::to_wstring(d->untitledId) : L"Untitled";
}

void updateTitle() {
    Doc* d = curDoc();
    std::wstring t = d ? docTitle(d) + L" - " + kAppName : kAppName;
    SetWindowTextW(g_hwnd, t.c_str());
}

Doc* createDoc() {
    auto d = std::make_unique<Doc>();
    HWND e = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_DOC, g_inst, nullptr);
    d->edit = e;
    SendMessageW(e, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    SendMessageW(e, EM_EXLIMITTEXT, 0, 0x7FFFFFF0);
    SendMessageW(e, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
    SendMessageW(e, EM_SETBKGNDCOLOR, 0, cBg);
    SendMessageW(e, EM_SETUNDOLIMIT, 1000, 0);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(12), S(12)));
    SetWindowTheme(e, L"DarkMode_Explorer", nullptr);
    SetWindowSubclass(e, EditSub, 1, 0);
    RevokeDragDrop(e);
    DragAcceptFiles(e, TRUE);
    applyFormat(e);
    applyWrap(d.get());
    applyZoom(d.get());

    int w = clientRc().right;
    int idx = (int)g_docs.size();
    d->aw = 0;
    d->ax = tabTargetX(idx, w);
    Doc* raw = d.get();
    g_docs.push_back(std::move(d));
    ensureAnim();
    return raw;
}

void newDoc() {
    Doc* d = createDoc();
    d->untitledId = nextUntitledId();
    DeleteFileW(backupPath(d->untitledId).c_str());
    setCur((int)g_docs.size() - 1);
    saveSession();
}

bool loadFileInto(Doc* d, const std::wstring& path) {
    std::string b;
    if (!readAll(path, b)) return false;
    Enc e; Eol l;
    std::wstring w = decodeBytes(b, e, l);
    d->enc = e; d->eol = l; d->path = path;
    setDocText(d, w);
    d->mtime = getMtime(path);
    d->dirty = d->pending = false;
    return true;
}

std::wstring fullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)_countof(buf), buf, nullptr);
    return (n > 0 && n < _countof(buf)) ? std::wstring(buf) : p;
}

void openFile(const std::wstring& raw) {
    std::wstring path = fullPath(raw);
    for (size_t i = 0; i < g_docs.size(); i++)
        if (!g_docs[i]->path.empty() &&
            CompareStringOrdinal(g_docs[i]->path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) {
            setCur((int)i);
            return;
        }
    std::string probe;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, (L"Cannot find the file:\n" + path).c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    Doc* reuse = nullptr;
    if (g_docs.size() == 1) {
        Doc* d = g_docs[0].get();
        if (d->path.empty() && !d->dirty && textLen(d->edit) == 0) reuse = d;
    }
    Doc* d = reuse ? reuse : createDoc();
    if (!loadFileInto(d, path)) {
        MessageBoxW(g_hwnd, (L"Could not open the file:\n" + path).c_str(), kAppName, MB_OK | MB_ICONWARNING);
        if (!reuse) { DestroyWindow(d->edit); g_docs.pop_back(); }
        return;
    }
    if (reuse) { DeleteFileW(backupPath(d->untitledId).c_str()); d->untitledId = 0; }
    setCur(indexOf(d));
    saveSession();
}

bool saveDoc(Doc* d) {
    std::wstring target = d->path.empty() ? backupPath(d->untitledId) : d->path;
    std::string bytes = encodeText(getText(d->edit), d->enc, d->eol);
    bool ok = writeAll(target, bytes);
    d->pending = false;
    if (ok) {
        d->dirty = false;
        if (!d->path.empty()) d->mtime = getMtime(d->path);
    } else {
        toast(L"Could not save " + docTitle(d), cWarn, 6000);
    }
    g_statusDirty = true;
    invalidateTop();
    return ok;
}

bool saveAs(Doc* d) {
    wchar_t buf[MAX_PATH * 2] = {};
    std::wstring initial = d->path.empty() ? docTitle(d) + L".txt" : d->path;
    wcsncpy_s(buf, initial.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)_countof(buf);
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return false;
    if (d->path.empty()) DeleteFileW(backupPath(d->untitledId).c_str());
    d->path = buf;
    d->untitledId = 0;
    bool ok = saveDoc(d);
    updateTitle();
    saveSession();
    invalidateTop();
    return ok;
}

void setCur(int i) {
    if (i < 0 || i >= (int)g_docs.size()) return;
    if (Doc* o = curDoc()) {
        if (g_cur != i) {
            if (o->pending) saveDoc(o);
            ShowWindow(o->edit, SW_HIDE);
        }
    }
    g_cur = i;
    Doc* d = g_docs[i].get();
    layoutEdit(d);
    ShowWindow(d->edit, SW_SHOW);
    SetFocus(d->edit);
    updateTitle();
    g_statusDirty = true;
    ensureAnim();
    invalidateTop();
    invalidateStatus();
}

bool closeDoc(int i) {
    Doc* d = g_docs[i].get();
    if (d->path.empty() && textLen(d->edit) > 0) {
        int r = MessageBoxW(g_hwnd, L"Save this document before closing?\n\nIt has not been saved to a file yet.",
                            kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES && !saveAs(d)) return false;
    } else if (d->pending || d->dirty) {
        saveDoc(d);
    }
    if (d->path.empty()) DeleteFileW(backupPath(d->untitledId).c_str());
    bool wasCur = (i == g_cur);
    DestroyWindow(d->edit);
    g_docs.erase(g_docs.begin() + i);
    g_hotTab = -1; g_hotClose = false; g_pressClose = nullptr;
    if (g_docs.empty()) {
        g_cur = -1;
        saveSession();
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    if (wasCur) { g_cur = -1; setCur((std::min)(i, (int)g_docs.size() - 1)); }
    else if (i < g_cur) g_cur--;
    ensureAnim();
    saveSession();
    invalidateTop();
    return true;
}

void saveAllPending() {
    for (auto& d : g_docs) if (d->pending) saveDoc(d.get());
}

void checkExternalChanges() {
    for (auto& up : g_docs) {
        Doc* d = up.get();
        if (d->path.empty() || d->dirty) continue;
        FILETIME ft = getMtime(d->path);
        if ((ft.dwHighDateTime | ft.dwLowDateTime) == 0) continue;
        if (CompareFileTime(&ft, &d->mtime) == 0) continue;
        CHARRANGE sel{};
        SendMessageW(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
        int topLine = (int)SendMessageW(d->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (loadFileInto(d, d->path)) {
            SendMessageW(d->edit, EM_EXSETSEL, 0, (LPARAM)&sel);
            SendMessageW(d->edit, EM_LINESCROLL, 0, topLine - (int)SendMessageW(d->edit, EM_GETFIRSTVISIBLELINE, 0, 0));
            if (d == curDoc()) toast(L"Reloaded - file changed on disk");
        }
    }
}

// ---------------------------------------------------------------- session

void saveSession() {
    std::string s;
    s += "wrap=" + std::string(g_wrap ? "1" : "0") + "\n";
    s += "status=" + std::string(g_showStatus ? "1" : "0") + "\n";
    s += "zoom=" + std::to_string(g_zoom) + "\n";
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (g_hwnd && GetWindowPlacement(g_hwnd, &wp)) {
        bool max = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED);
        // Maximized: the restored rect (workspace coords). Otherwise the real on-screen rect, which also
        // captures snapped positions (GetWindowPlacement reports the pre-snap rect for those).
        RECT r = wp.rcNormalPosition;
        if (!max) {
            if (!IsIconic(g_hwnd)) GetWindowRect(g_hwnd, &r);
            else {
                MONITORINFO mi{ sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi))
                    OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
            }
        }
        s += "win=" + std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," +
             std::to_string(r.bottom) + "," + (max ? "1" : "0") + "\n";
    }
    s += "active=" + std::to_string(g_cur < 0 ? 0 : g_cur) + "\n";
    for (auto& d : g_docs) {
        if (d->path.empty()) s += "doc|U|" + std::to_string(d->untitledId) + "\n";
        else s += "doc|F|" + toUtf8(d->path) + "\n";
    }
    writeAll(g_dataDir + L"\\session.txt", s);
}

void restoreSession() {
    std::string raw;
    if (!readAll(g_dataDir + L"\\session.txt", raw)) return;
    std::wstring w = fromUtf8(raw);
    int active = 0;
    size_t pos = 0;
    while (pos < w.size()) {
        size_t e = w.find(L'\n', pos);
        if (e == std::wstring::npos) e = w.size();
        std::wstring line = w.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"wrap=", 0) == 0) g_wrap = line.substr(5) != L"0";
        else if (line.rfind(L"status=", 0) == 0) g_showStatus = line.substr(7) != L"0";
        else if (line.rfind(L"zoom=", 0) == 0) g_zoom = iclamp(_wtoi(line.c_str() + 5), 25, 500);
        else if (line.rfind(L"active=", 0) == 0) active = _wtoi(line.c_str() + 7);
        else if (line.rfind(L"win=", 0) == 0) {
            WINDOWPLACEMENT wp{ sizeof(wp) };
            int mx = 0;
            RECT& r = wp.rcNormalPosition;
            if (swscanf_s(line.c_str() + 4, L"%ld,%ld,%ld,%ld,%d", &r.left, &r.top, &r.right, &r.bottom, &mx) == 5 &&
                r.right - r.left >= 300 && r.bottom - r.top >= 200 &&
                MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                if (mx) {
                    wp.showCmd = SW_HIDE;
                    SetWindowPlacement(g_hwnd, &wp);
                } else {
                    SetWindowPos(g_hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                g_startMax = mx != 0;
                g_placed = true;
            }
        }
        else if (line.rfind(L"doc|F|", 0) == 0) {
            std::wstring p = line.substr(6);
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            Doc* d = createDoc();
            if (!loadFileInto(d, p)) { DestroyWindow(d->edit); g_docs.pop_back(); }
        } else if (line.rfind(L"doc|U|", 0) == 0) {
            int id = _wtoi(line.c_str() + 6);
            if (id <= 0) continue;
            Doc* d = createDoc();
            d->untitledId = id;
            std::string b;
            if (readAll(backupPath(id), b)) {
                Enc en; Eol el;
                setDocText(d, decodeBytes(b, en, el));
                d->enc = Enc::Utf8; d->eol = el;
            }
        }
    }
    if (!g_docs.empty()) setCur(iclamp(active, 0, (int)g_docs.size() - 1));
}

// ---------------------------------------------------------------- zoom / wrap

void setZoom(int pct) {
    g_zoom = iclamp(pct, 25, 500);
    for (auto& d : g_docs) applyZoom(d.get());
    g_statusDirty = true;
    saveSession();
}
void setWrap(bool on) {
    g_wrap = on;
    for (auto& d : g_docs) applyWrap(d.get());
    g_statusDirty = true;
    saveSession();
}

// ---------------------------------------------------------------- find / replace

std::wstring windowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

void setFindMsg(const std::wstring& m, bool bad) { g_findMsg = m; g_findMsgBad = bad; invalidateTop(); }

bool doFind(bool down, bool fromSelStart) {
    Doc* d = curDoc();
    if (!d) return false;
    std::wstring q = windowText(g_find);
    if (q.empty()) { setFindMsg(L"", false); return false; }
    CHARRANGE sel{};
    SendMessageW(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    LONG len = textLen(d->edit);
    WPARAM flags = (down ? FR_DOWN : 0) | (g_matchCase ? FR_MATCHCASE : 0) | (g_wholeWord ? FR_WHOLEWORD : 0);
    FINDTEXTEXW ft{};
    ft.lpstrText = q.c_str();
    LONG start = down ? (fromSelStart ? sel.cpMin : sel.cpMax) : sel.cpMin;
    ft.chrg.cpMin = start; ft.chrg.cpMax = down ? -1 : 0;
    bool wrapped = false;
    LRESULT r = SendMessageW(d->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
    if (r < 0 && g_wrapAround) {
        ft.chrg.cpMin = down ? 0 : len; ft.chrg.cpMax = down ? -1 : 0;
        r = SendMessageW(d->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft);
        wrapped = r >= 0;
    }
    if (r < 0) { setFindMsg(L"No results", true); return false; }
    SendMessageW(d->edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessageW(d->edit, EM_SCROLLCARET, 0, 0);
    setFindMsg(wrapped ? L"Wrapped around" : L"", false);
    return true;
}

void replaceOne() {
    Doc* d = curDoc();
    if (!d) return;
    std::wstring q = windowText(g_find);
    if (q.empty()) return;
    CHARRANGE sel{};
    SendMessageW(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    bool match = false;
    if (sel.cpMax - sel.cpMin == (LONG)q.size()) {
        std::wstring s(q.size() + 1, L'\0');
        SendMessageW(d->edit, EM_GETSELTEXT, 0, (LPARAM)s.data());
        s.resize(q.size());
        match = CompareStringOrdinal(s.c_str(), (int)s.size(), q.c_str(), (int)q.size(), !g_matchCase) == CSTR_EQUAL;
    }
    if (match) SendMessageW(d->edit, EM_REPLACESEL, TRUE, (LPARAM)windowText(g_repl).c_str());
    doFind(true, false);
}

void replaceAll() {
    Doc* d = curDoc();
    if (!d) return;
    std::wstring q = windowText(g_find), rep = windowText(g_repl);
    if (q.empty()) return;
    WPARAM flags = FR_DOWN | (g_matchCase ? FR_MATCHCASE : 0) | (g_wholeWord ? FR_WHOLEWORD : 0);
    int count = 0;
    LONG pos = 0;
    d->loading = true;
    SendMessageW(d->edit, WM_SETREDRAW, FALSE, 0);
    for (;;) {
        FINDTEXTEXW ft{};
        ft.lpstrText = q.c_str();
        ft.chrg.cpMin = pos; ft.chrg.cpMax = -1;
        if (SendMessageW(d->edit, EM_FINDTEXTEXW, flags, (LPARAM)&ft) < 0) break;
        SendMessageW(d->edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
        SendMessageW(d->edit, EM_REPLACESEL, TRUE, (LPARAM)rep.c_str());
        pos = ft.chrgText.cpMin + (LONG)rep.size();
        count++;
    }
    SendMessageW(d->edit, EM_SETSEL, pos, pos);
    SendMessageW(d->edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(d->edit, nullptr, TRUE);
    d->loading = false;
    if (count > 0) {
        d->dirty = d->pending = true;
        d->lastChange = d->firstPending = GetTickCount();
        g_statusDirty = true;
    }
    setFindMsg(count ? std::to_wstring(count) + L" replaced" : L"No results", count == 0);
    SendMessageW(d->edit, EM_SCROLLCARET, 0, 0);
}

void showFind(bool replace) {
    Doc* d = curDoc();
    if (!d) return;
    bool wasVisible = g_findVis;
    g_findVis = true;
    g_replVis = replace || (wasVisible && g_replVis);
    CHARRANGE sel{};
    SendMessageW(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    if (sel.cpMax > sel.cpMin && sel.cpMax - sel.cpMin <= 200) {
        std::wstring s(sel.cpMax - sel.cpMin + 1, L'\0');
        SendMessageW(d->edit, EM_GETSELTEXT, 0, (LPARAM)s.data());
        s.resize(sel.cpMax - sel.cpMin);
        if (s.find(L'\r') == std::wstring::npos && s.find(L'\v') == std::wstring::npos) SetWindowTextW(g_find, s.c_str());
    }
    layout();
    SetFocus(g_find);
    SendMessageW(g_find, EM_SETSEL, 0, -1);
}

void closeFind() {
    if (!g_findVis) return;
    g_findVis = false; g_replVis = false;
    setFindMsg(L"", false);
    layout();
    if (Doc* d = curDoc()) SetFocus(d->edit);
}

// ---------------------------------------------------------------- commands

void cmdOpen() {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    const wchar_t* first = buf.data();
    const wchar_t* next = first + wcslen(first) + 1;
    if (*next == 0) { openFile(first); return; }
    std::wstring dir = first;
    for (const wchar_t* n = next; *n; n += wcslen(n) + 1) openFile(dir + L"\\" + n);
}

void showMainMenu() {
    HMENU m = CreatePopupMenu(), f = CreatePopupMenu(), e = CreatePopupMenu(), v = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, IDM_NEW, L"New tab\tCtrl+N");
    AppendMenuW(f, MF_STRING, IDM_OPEN, L"Open...\tCtrl+O");
    AppendMenuW(f, MF_STRING, IDM_SAVE, L"Save\tCtrl+S");
    AppendMenuW(f, MF_STRING, IDM_SAVEAS, L"Save as...\tCtrl+Shift+S");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, IDM_CLOSETAB, L"Close tab\tCtrl+W");
    AppendMenuW(f, MF_STRING, IDM_EXIT, L"Exit");
    AppendMenuW(e, MF_STRING, IDM_UNDO, L"Undo\tCtrl+Z");
    AppendMenuW(e, MF_STRING, IDM_REDO, L"Redo\tCtrl+Y");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(e, MF_STRING, IDM_CUT, L"Cut\tCtrl+X");
    AppendMenuW(e, MF_STRING, IDM_COPY, L"Copy\tCtrl+C");
    AppendMenuW(e, MF_STRING, IDM_PASTE, L"Paste\tCtrl+V");
    AppendMenuW(e, MF_STRING, IDM_SELALL, L"Select all\tCtrl+A");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(e, MF_STRING, IDM_FIND, L"Find...\tCtrl+F");
    AppendMenuW(e, MF_STRING, IDM_FINDNEXT, L"Find next\tF3");
    AppendMenuW(e, MF_STRING, IDM_FINDPREV, L"Find previous\tShift+F3");
    AppendMenuW(e, MF_STRING, IDM_REPLACE, L"Replace...\tCtrl+H");
    AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(e, MF_STRING, IDM_DATETIME, L"Time/Date\tF5");
    AppendMenuW(v, MF_STRING, IDM_ZOOMIN, L"Zoom in\tCtrl++");
    AppendMenuW(v, MF_STRING, IDM_ZOOMOUT, L"Zoom out\tCtrl+-");
    AppendMenuW(v, MF_STRING, IDM_ZOOMRESET, L"Restore default zoom\tCtrl+0");
    AppendMenuW(v, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(v, MF_STRING | (g_wrap ? MF_CHECKED : 0), IDM_WRAP, L"Word wrap\tAlt+Z");
    AppendMenuW(v, MF_STRING | (g_showStatus ? MF_CHECKED : 0), IDM_STATUSBAR, L"Status bar");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)f, L"File");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)e, L"Edit");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)v, L"View");
    const RectF& r = g_btns[B_MENU].r;
    POINT p = { (LONG)(r.X + r.Width), (LONG)(r.Y + r.Height + Sf(2)) };
    ClientToScreen(g_hwnd, &p);
    TrackPopupMenuEx(m, TPM_RIGHTALIGN | TPM_TOPALIGN, p.x, p.y, g_hwnd, nullptr);
    DestroyMenu(m);
    g_hot = -1; ensureAnim();
}

void onCommand(int id) {
    Doc* d = curDoc();
    HWND e = d ? d->edit : nullptr;
    switch (id) {
    case IDM_NEW: newDoc(); break;
    case IDM_OPEN: cmdOpen(); break;
    case IDM_SAVE:
        if (d) {
            if (d->path.empty()) saveAs(d);
            else if (saveDoc(d)) toast(L"Saved");
        }
        break;
    case IDM_SAVEAS: if (d) saveAs(d); break;
    case IDM_CLOSETAB: if (d) closeDoc(g_cur); break;
    case IDM_EXIT: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    case IDM_NEXTTAB: if (g_docs.size() > 1) setCur((g_cur + 1) % (int)g_docs.size()); break;
    case IDM_PREVTAB: if (g_docs.size() > 1) setCur((g_cur + (int)g_docs.size() - 1) % (int)g_docs.size()); break;
    case IDM_FIND: showFind(false); break;
    case IDM_REPLACE: showFind(true); break;
    case IDM_FINDNEXT: if (!g_findVis) showFind(false); else doFind(true, false); break;
    case IDM_FINDPREV: if (!g_findVis) showFind(false); else doFind(false, false); break;
    case IDM_REPLACEONE: replaceOne(); break;
    case IDM_REPLACEALL: replaceAll(); break;
    case IDM_CLOSEFIND: closeFind(); break;
    case IDM_UNDO: if (e) SendMessageW(e, EM_UNDO, 0, 0); break;
    case IDM_REDO: if (e) SendMessageW(e, EM_REDO, 0, 0); break;
    case IDM_CUT: if (e) SendMessageW(e, WM_CUT, 0, 0); break;
    case IDM_COPY: if (e) SendMessageW(e, WM_COPY, 0, 0); break;
    case IDM_PASTE: if (e) SendMessageW(e, WM_PASTE, 0, 0); break;
    case IDM_SELALL: if (e) SendMessageW(e, EM_SETSEL, 0, -1); break;
    case IDM_DATETIME:
        if (e) {
            wchar_t t[64], dt[64];
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, t, 64);
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, dt, 64, nullptr);
            std::wstring s = std::wstring(t) + L" " + dt;
            SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
        }
        break;
    case IDM_ZOOMIN: setZoom(g_zoom + 10); break;
    case IDM_ZOOMOUT: setZoom(g_zoom - 10); break;
    case IDM_ZOOMRESET: setZoom(100); break;
    case IDM_WRAP: setWrap(!g_wrap); break;
    case IDM_STATUSBAR: g_showStatus = !g_showStatus; layout(); saveSession(); break;
    case IDM_MATCHCASE: g_matchCase = !g_matchCase; doFind(true, true); break;
    case IDM_WHOLEWORD: g_wholeWord = !g_wholeWord; doFind(true, true); break;
    case IDM_WRAPAROUND: g_wrapAround = !g_wrapAround; invalidateTop(); break;
    case IDM_MENU: showMainMenu(); break;
    default:
        if (id >= IDM_TAB1 && id < IDM_TAB1 + 9) {
            int i = id - IDM_TAB1;
            if (i == 8) i = (int)g_docs.size() - 1;
            if (i < (int)g_docs.size()) setCur(i);
        }
    }
}

// ---------------------------------------------------------------- status

void updateStatus() {
    g_statusDirty = false;
    Doc* d = curDoc();
    if (!d) return;
    CHARRANGE sel{};
    SendMessageW(d->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    g_stSel = (int)(sel.cpMax - sel.cpMin);
    g_stChars = (int)textLen(d->edit);
    LONG pos = sel.cpMax;
    if (g_stChars < 20000000) {
        std::wstring buf(pos + 1, L'\0');
        TEXTRANGEW tr{ { 0, pos }, buf.data() };
        SendMessageW(d->edit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        buf.resize(pos);
        int line = 1; size_t lastCr = std::wstring::npos;
        for (size_t i = 0; i < buf.size(); i++)
            if (buf[i] == L'\r') { line++; lastCr = i; }
        g_stLine = line;
        g_stCol = (int)(pos - (lastCr == std::wstring::npos ? 0 : (LONG)lastCr + 1)) + 1;
    } else {
        LRESULT ln = SendMessageW(d->edit, EM_LINEFROMCHAR, pos, 0);
        g_stLine = (int)ln + 1;
        g_stCol = (int)(pos - SendMessageW(d->edit, EM_LINEINDEX, ln, 0)) + 1;
    }
    invalidateStatus();
}

void onTick() {
    DWORD now = GetTickCount();
    for (auto& d : g_docs)
        if (d->pending && (now - d->lastChange >= 700 || now - d->firstPending >= 3000)) saveDoc(d.get());
    static DWORD lastExtCheck = 0;
    if (now - lastExtCheck >= 500) { lastExtCheck = now; checkExternalChanges(); }
    if (g_msgUntil && (int)(now - g_msgUntil) >= 0) { g_msgUntil = 0; g_msg.clear(); g_statusDirty = true; }
    if (g_statusDirty) updateStatus();
}

void onDocChanged(Doc* d) {
    if (d->loading) return;
    DWORD now = GetTickCount();
    if (!d->pending) d->firstPending = now;
    d->dirty = d->pending = true;
    d->lastChange = now;
    g_statusDirty = true;
    invalidateTop();
}

// ---------------------------------------------------------------- painting

void rrPath(GraphicsPath& p, RectF r, float tl, float tr, float br, float bl) {
    float x = r.X, y = r.Y, w = r.Width, h = r.Height;
    if (tl > 0) p.AddArc(x, y, 2 * tl, 2 * tl, 180, 90);
    p.AddLine(x + tl, y, x + w - tr, y);
    if (tr > 0) p.AddArc(x + w - 2 * tr, y, 2 * tr, 2 * tr, 270, 90);
    p.AddLine(x + w, y + tr, x + w, y + h - br);
    if (br > 0) p.AddArc(x + w - 2 * br, y + h - 2 * br, 2 * br, 2 * br, 0, 90);
    p.AddLine(x + w - br, y + h, x + bl, y + h);
    if (bl > 0) p.AddArc(x, y + h - 2 * bl, 2 * bl, 2 * bl, 90, 90);
    p.CloseFigure();
}
void fillRR(Graphics& g, RectF r, float rad, Color c) {
    GraphicsPath p; rrPath(p, r, rad, rad, rad, rad);
    SolidBrush b(c); g.FillPath(&b, &p);
}
void strokeRR(Graphics& g, RectF r, float rad, Color c, float w = 1.0f) {
    r.X += 0.5f; r.Y += 0.5f; r.Width -= 1; r.Height -= 1;
    GraphicsPath p; rrPath(p, r, rad, rad, rad, rad);
    Pen pen(c, w); g.DrawPath(&pen, &p);
}
void drawText(Graphics& g, const std::wstring& s, RectF r, COLORREF c, Font* f, StringFormat* fmt, BYTE a = 255) {
    SolidBrush b(C(c, a));
    g.DrawString(s.c_str(), (INT)s.size(), f, r, fmt, &b);
}
float textWidth(Graphics& g, const std::wstring& s, Font* f) {
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
    RectF out;
    g.MeasureString(s.c_str(), (INT)s.size(), f, PointF(0, 0), &fmt, &out);
    return out.Width;
}

void drawBtn(Graphics& g, const Btn& b, bool pressed) {
    if (!b.vis) return;
    bool on = b.flag && *b.flag;
    float rad = Sf(5);
    COLORREF base = (b.kind == K_TEXT) ? cField : cBg;
    COLORREF fill = mix(base, cBtnHover, b.hov);
    if (pressed) fill = mix(fill, cBg, 0.5f);
    if (b.kind == K_TOGGLE && on) fillRR(g, b.r, rad, C(cAccent, (BYTE)(50 + 30 * b.hov)));
    else fillRR(g, b.r, rad, C(fill));
    if (b.kind == K_TEXT) strokeRR(g, b.r, rad, C(cBorder));
    if (b.kind == K_TOGGLE) strokeRR(g, b.r, rad, on ? C(cAccent, 180) : C(cBorder));

    float cx = b.r.X + b.r.Width / 2, cy = b.r.Y + b.r.Height / 2, u = Sf(1);
    Pen pen(C(cText), Sf(1.3f));
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    switch (b.kind) {
    case K_TEXT:
    case K_TOGGLE:
        drawText(g, b.label, b.r, (b.kind == K_TOGGLE && on) ? cAccent : cText, g_uiFont.get(), g_fCenter.get());
        break;
    case K_UP: { PointF p[3] = { {cx - 5 * u, cy + 2.5f * u}, {cx, cy - 2.5f * u}, {cx + 5 * u, cy + 2.5f * u} }; g.DrawLines(&pen, p, 3); break; }
    case K_DOWN: { PointF p[3] = { {cx - 5 * u, cy - 2.5f * u}, {cx, cy + 2.5f * u}, {cx + 5 * u, cy - 2.5f * u} }; g.DrawLines(&pen, p, 3); break; }
    case K_CLOSE:
        g.DrawLine(&pen, cx - 4.5f * u, cy - 4.5f * u, cx + 4.5f * u, cy + 4.5f * u);
        g.DrawLine(&pen, cx - 4.5f * u, cy + 4.5f * u, cx + 4.5f * u, cy - 4.5f * u);
        break;
    case K_PLUS:
        g.DrawLine(&pen, cx - 5.5f * u, cy, cx + 5.5f * u, cy);
        g.DrawLine(&pen, cx, cy - 5.5f * u, cx, cy + 5.5f * u);
        break;
    case K_MENU:
        for (int i = -1; i <= 1; i++) g.DrawLine(&pen, cx - 6.5f * u, cy + i * 5 * u, cx + 6.5f * u, cy + i * 5 * u);
        break;
    }
}

void drawTab(Graphics& g, int i) {
    Doc* d = g_docs[i].get();
    bool sel = (i == g_cur);
    float top = Sf(6);
    RectF r(d->ax, top, d->aw, Sf(kTabH) - top);
    if (r.Width < 2) return;
    if (sel) {
        GraphicsPath p; rrPath(p, r, Sf(8), Sf(8), 0, 0);
        SolidBrush b(C(cBg)); g.FillPath(&b, &p);
    } else if (d->hov > 0.01f) {
        RectF hr(r.X + Sf(2), r.Y + Sf(2), r.Width - Sf(4), r.Height - Sf(6));
        fillRR(g, hr, Sf(6), C(mix(cStrip, cTabHover, d->hov)));
    }
    bool room = r.Width >= Sf(90) || sel;
    RectF cr = tabCloseRect(d);
    float closeA = (d->dirty ? 1.f : (sel ? 1.f : d->hov));
    if (room && closeA > 0.02f) {
        if (d->chov > 0.01f) fillRR(g, cr, Sf(5), Color((BYTE)(28 * d->chov), 255, 255, 255));
        float cx = cr.X + cr.Width / 2, cy = cr.Y + cr.Height / 2, u = Sf(1);
        if (d->dirty && d->chov < 0.5f) {
            SolidBrush b(C(cDim)); g.FillEllipse(&b, cx - 3 * u, cy - 3 * u, 6 * u, 6 * u);
        } else {
            Pen pen(C(cText, (BYTE)(255 * (std::max)(closeA, d->chov))), Sf(1.2f));
            pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
            g.DrawLine(&pen, cx - 4 * u, cy - 4 * u, cx + 4 * u, cy + 4 * u);
            g.DrawLine(&pen, cx - 4 * u, cy + 4 * u, cx + 4 * u, cy - 4 * u);
        }
    }
    float rightPad = room ? Sf(34) : Sf(10);
    RectF tr(r.X + Sf(12), r.Y, (std::max)(0.f, r.Width - Sf(12) - rightPad), r.Height - Sf(2));
    drawText(g, docTitle(d), tr, mix(cDim, cText, sel ? 1.f : d->hov), g_uiFont.get(), g_fLeft.get());
}

void drawField(Graphics& g, const RectF& f, HWND edit) {
    fillRR(g, f, Sf(5), C(cField));
    bool focus = GetFocus() == edit;
    strokeRR(g, f, Sf(5), focus ? C(cAccent) : C(cBorder), focus ? Sf(1.5f) : 1.f);
}

void paintAll(Graphics& g, int w, int h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    layoutStrip(w);

    { SolidBrush b(C(cStrip)); g.FillRectangle(&b, 0, 0, w, h); }

    // tabs: inactive first, then the active one, then a dragged one on top
    for (int i = 0; i < (int)g_docs.size(); i++) if (i != g_cur && i != g_dragIdx) drawTab(g, i);
    if (g_cur >= 0 && g_cur != g_dragIdx) drawTab(g, g_cur);
    if (g_dragIdx >= 0 && g_dragIdx < (int)g_docs.size()) drawTab(g, g_dragIdx);

    // find / replace panel
    if (g_findVis) {
        float top = Sf(kTabH), fh = (float)findH();
        { SolidBrush b(C(cBg)); g.FillRectangle(&b, 0.f, top, (float)w, fh); }
        { SolidBrush b(C(cSep)); g.FillRectangle(&b, 0.f, top + fh - 1, (float)w, 1.f); }
        drawField(g, g_fieldFind, g_find);
        if (g_replVis) drawField(g, g_fieldRepl, g_repl);
        if (!g_findMsg.empty()) {
            float rx = g_btns[B_CLOSEFIND].r.X - Sf(12);
            RectF mr(rx - Sf(160), g_btns[B_CLOSEFIND].r.Y, Sf(160), g_btns[B_CLOSEFIND].r.Height);
            drawText(g, g_findMsg, mr, g_findMsgBad ? cWarn : cDim, g_uiFont.get(), g_fRight.get());
        }
    }
    for (size_t i = 0; i < g_btns.size(); i++) drawBtn(g, g_btns[i], g_pressBtn == (int)i && g_hot == (int)i);

    // status bar
    if (g_showStatus) {
        float sh = (float)statusH(), top = h - sh;
        Doc* d = curDoc();
        Font* f = g_smallFont.get();
        std::wstring left = g_msg.empty()
            ? L"Ln " + std::to_wstring(g_stLine) + L", Col " + std::to_wstring(g_stCol) +
              (g_stSel ? L"  (" + std::to_wstring(g_stSel) + L" selected)" : L"")
            : g_msg;
        float x = Sf(16);
        float lw = textWidth(g, left, f);
        drawText(g, left, RectF(x, top, lw + Sf(20), sh), g_msg.empty() ? cDim : g_msgColor, f, g_fLeft.get());
        x += lw + Sf(24);
        std::wstring chars = std::to_wstring(g_stChars) + (g_stChars == 1 ? L" character" : L" characters");
        drawText(g, chars, RectF(x, top, Sf(200), sh), cDim, f, g_fLeft.get());

        if (d) {
            std::wstring items[4] = {
                std::to_wstring(g_zoom) + L"%",
                d->eol == Eol::Crlf ? L"Windows (CRLF)" : L"Unix (LF)",
                d->enc == Enc::Utf8 ? L"UTF-8" : d->enc == Enc::Utf8Bom ? L"UTF-8 with BOM" : d->enc == Enc::Utf16 ? L"UTF-16 LE" : L"ANSI",
                d->dirty ? L"Saving..." : L"Saved" };
            float rx = (float)w - Sf(16);
            for (int i = 3; i >= 0; i--) {
                float tw = textWidth(g, items[i], f) + Sf(16);
                RectF r(rx - tw, top, tw, sh);
                if (i == 1) g_stEol = RectF(r.X - Sf(8), top, r.Width + Sf(16), sh);
                if (i == 3) {
                    SolidBrush b(d->dirty ? C(cAccent) : C(RGB(0x6C, 0xCB, 0x5F)));
                    g.FillEllipse(&b, r.X - Sf(14), top + sh / 2 - Sf(3), Sf(6), Sf(6));
                }
                drawText(g, items[i], r, cDim, f, g_fRight.get());
                rx -= tw + Sf(i == 3 ? 24 : 12);
            }
        }
    }
}

void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w > 0 && h > 0) {
        if (!g_mdc || g_bw != w || g_bh != h) {
            if (!g_mdc) g_mdc = CreateCompatibleDC(hdc);
            if (g_bmp) { SelectObject(g_mdc, g_oldBmp); DeleteObject(g_bmp); }
            g_bmp = CreateCompatibleBitmap(hdc, w, h);
            g_oldBmp = (HBITMAP)SelectObject(g_mdc, g_bmp);
            g_bw = w; g_bh = h;
        }
        { Graphics g(g_mdc); paintAll(g, w, h); }
        BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, g_mdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- mouse

enum HitKind { H_NONE, H_TAB, H_TABCLOSE, H_BTN, H_EOL };
struct Hit { HitKind kind = H_NONE; int idx = -1; };

Hit hitTest(int x, int y) {
    layoutStrip(clientRc().right);
    Hit h;
    float fx = (float)x, fy = (float)y;
    for (int i = 0; i < (int)g_btns.size(); i++)
        if (g_btns[i].vis && g_btns[i].r.Contains(fx, fy)) { h.kind = H_BTN; h.idx = i; return h; }
    if (y < S(kTabH)) {
        for (int i = 0; i < (int)g_docs.size(); i++) {
            Doc* d = g_docs[i].get();
            if (fx >= d->ax && fx < d->ax + d->aw && fy >= Sf(6)) {
                bool room = d->aw >= Sf(90) || i == g_cur;
                h.kind = (room && tabCloseRect(d).Contains(fx, fy)) ? H_TABCLOSE : H_TAB;
                h.idx = i;
                return h;
            }
        }
    } else if (g_showStatus && y >= clientRc().bottom - statusH() && g_stEol.Contains(fx, fy)) {
        h.kind = H_EOL;
    }
    return h;
}

void updateHover(int x, int y) {
    Hit h = hitTest(x, y);
    int hotBtn = h.kind == H_BTN ? h.idx : -1;
    int hotTab = (h.kind == H_TAB || h.kind == H_TABCLOSE) ? h.idx : -1;
    bool hotClose = h.kind == H_TABCLOSE;
    if (hotBtn != g_hot || hotTab != g_hotTab || hotClose != g_hotClose) {
        g_hot = hotBtn; g_hotTab = hotTab; g_hotClose = hotClose;
        ensureAnim();
    }
}

void onMouseMove(int x, int y) {
    if (!g_tracking) {
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, g_hwnd, 0 };
        TrackMouseEvent(&t);
        g_tracking = true;
    }
    if (g_dragIdx >= 0 && g_dragIdx < (int)g_docs.size()) {
        if (!g_dragging && std::abs(x - g_dragStartX) > S(5)) g_dragging = true;
        if (g_dragging) {
            int w = clientRc().right;
            float tw = tabTargetW(w);
            Doc* d = g_docs[g_dragIdx].get();
            float maxX = Sf(8) + (g_docs.size() - 1) * tw;
            d->ax = std::clamp(x - g_dragOff, Sf(8), maxX);
            int target = iclamp((int)((d->ax + tw / 2 - Sf(8)) / tw), 0, (int)g_docs.size() - 1);
            if (target != g_dragIdx) {
                auto moved = std::move(g_docs[g_dragIdx]);
                g_docs.erase(g_docs.begin() + g_dragIdx);
                g_docs.insert(g_docs.begin() + target, std::move(moved));
                g_cur = target; g_dragIdx = target;
            }
            ensureAnim();
            invalidateTop();
        }
        return;
    }
    updateHover(x, y);
}

void onLButtonDown(int x, int y, bool dbl) {
    Hit h = hitTest(x, y);
    switch (h.kind) {
    case H_BTN: g_pressBtn = h.idx; SetCapture(g_hwnd); invalidateTop(); break;
    case H_TABCLOSE: g_pressClose = g_docs[h.idx].get(); SetCapture(g_hwnd); break;
    case H_TAB: {
        setCur(h.idx);
        g_dragIdx = h.idx; g_dragStartX = x; g_dragging = false;
        g_dragOff = x - g_docs[h.idx]->ax;
        SetCapture(g_hwnd);
        break;
    }
    case H_EOL:
        if (Doc* d = curDoc()) {
            d->eol = d->eol == Eol::Crlf ? Eol::Lf : Eol::Crlf;
            d->dirty = d->pending = true;
            d->lastChange = d->firstPending = GetTickCount();
            g_statusDirty = true;
            invalidateStatus();
        }
        break;
    default:
        if (dbl && y < S(kTabH)) newDoc();
    }
}

void onLButtonUp(int x, int y) {
    // Take the press state first: ReleaseCapture() sends WM_CAPTURECHANGED, which clears it.
    int pressBtn = g_pressBtn; Doc* pressClose = g_pressClose;
    int dragIdx = g_dragIdx; bool wasDragging = g_dragging;
    g_pressBtn = -1; g_pressClose = nullptr; g_dragIdx = -1; g_dragging = false;
    if (GetCapture() == g_hwnd) ReleaseCapture();
    Hit h = hitTest(x, y);
    invalidateTop();
    if (pressBtn >= 0 && h.kind == H_BTN && h.idx == pressBtn) onCommand(g_btns[pressBtn].cmd);
    if (pressClose) {
        int i = indexOf(pressClose);
        if (i >= 0 && h.kind == H_TABCLOSE && h.idx == i) closeDoc(i);
    }
    if (dragIdx >= 0) {
        ensureAnim();
        if (wasDragging) saveSession();
    }
    updateHover(x, y);
}

// ---------------------------------------------------------------- window

void enableDarkMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto setMode = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2);  // ForceDark
    if (flush) flush();
}

void makeResources() {
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    FontFamily ff(L"Segoe UI");
    g_uiFont.reset(new Font(&ff, Sf(13), FontStyleRegular, UnitPixel));
    g_smallFont.reset(new Font(&ff, Sf(12), FontStyleRegular, UnitPixel));
    auto mk = [](StringAlignment a) {
        auto f = std::make_unique<StringFormat>(StringFormatFlagsNoWrap);
        f->SetAlignment(a); f->SetLineAlignment(StringAlignmentCenter);
        f->SetTrimming(StringTrimmingEllipsisCharacter);
        return f;
    };
    g_fLeft = mk(StringAlignmentNear); g_fCenter = mk(StringAlignmentCenter); g_fRight = mk(StringAlignmentFar);
    if (g_find) SendMessageW(g_find, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    if (g_repl) SendMessageW(g_repl, WM_SETFONT, (WPARAM)g_editFont, TRUE);
}

void applyDwm(HWND h) {
    BOOL dark = TRUE;
    COLORREF cap = cStrip, txt = cText;
    DwmSetWindowAttribute(h, 20 /*USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DwmSetWindowAttribute(h, 34 /*BORDER_COLOR*/, &cap, sizeof(cap));
    DwmSetWindowAttribute(h, 35 /*CAPTION_COLOR*/, &cap, sizeof(cap));
    DwmSetWindowAttribute(h, 36 /*TEXT_COLOR*/, &txt, sizeof(txt));
}

void onCreate(HWND hwnd) {
    g_hwnd = hwnd;
    g_dpi = GetDpiForWindow(hwnd);
    applyDwm(hwnd);
    g_fieldBrush = CreateSolidBrush(cField);

    DWORD es = WS_CHILD | ES_AUTOHSCROLL;
    g_find = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_FIND, g_inst, nullptr);
    g_repl = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_REPL, g_inst, nullptr);
    SetWindowSubclass(g_find, FindSub, 1, 0);
    SetWindowSubclass(g_repl, FindSub, 1, 0);
    Edit_SetCueBannerTextFocused(g_find, L"Find", TRUE);
    Edit_SetCueBannerTextFocused(g_repl, L"Replace", TRUE);
    makeResources();

    g_btns.resize(B_COUNT);
    g_btns[B_PLUS] = { IDM_NEW, K_PLUS, L"", nullptr };
    g_btns[B_MENU] = { IDM_MENU, K_MENU, L"", nullptr };
    g_btns[B_PREV] = { IDM_FINDPREV, K_UP, L"", nullptr };
    g_btns[B_NEXT] = { IDM_FINDNEXT, K_DOWN, L"", nullptr };
    g_btns[B_CASE] = { IDM_MATCHCASE, K_TOGGLE, L"Match case", &g_matchCase };
    g_btns[B_WORD] = { IDM_WHOLEWORD, K_TOGGLE, L"Whole word", &g_wholeWord };
    g_btns[B_WRAP] = { IDM_WRAPAROUND, K_TOGGLE, L"Wrap around", &g_wrapAround };
    g_btns[B_CLOSEFIND] = { IDM_CLOSEFIND, K_CLOSE, L"", nullptr };
    g_btns[B_REPLACE] = { IDM_REPLACEONE, K_TEXT, L"Replace", nullptr };
    g_btns[B_REPLALL] = { IDM_REPLACEALL, K_TEXT, L"Replace all", nullptr };

    SetTimer(hwnd, T_TICK, 100, nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: onCreate(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paintWindow(hwnd); return 0;
    case WM_SIZE: if (w != SIZE_MINIMIZED) layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = S(760); mm->ptMinTrackSize.y = S(420);
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        auto* r = (RECT*)l;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        makeResources();
        for (auto& d : g_docs) {
            SendMessageW(d->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(12), S(12)));
            applyFormat(d->edit);
        }
        layout();
        return 0;
    }
    case WM_SETFOCUS:
        if (Doc* d = curDoc()) SetFocus(d->edit);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            g_lastFocus = GetFocus();
            saveAllPending();
        } else {
            if (g_lastFocus && IsChild(hwnd, g_lastFocus) && IsWindowVisible(g_lastFocus)) SetFocus(g_lastFocus);
            else if (Doc* d = curDoc()) SetFocus(d->edit);
            checkExternalChanges();
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, cText);
        SetBkColor(dc, cField);
        return (LRESULT)g_fieldBrush;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (l != 0) {
            if (id == IDC_FIND || id == IDC_REPL) {
                if (code == EN_CHANGE && id == IDC_FIND) doFind(true, true);
                else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) invalidateTop();
            } else if (id == IDC_DOC && code == EN_CHANGE) {
                if (Doc* d = docByEdit((HWND)l)) onDocChanged(d);
            }
            return 0;
        }
        onCommand(id);
        return 0;
    }
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)l;
        if (nh->code == EN_SELCHANGE && docByEdit(nh->hwndFrom)) g_statusDirty = true;
        return 0;
    }
    case WM_MOUSEMOVE: onMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_MOUSELEAVE:
        g_tracking = false;
        if (GetCapture() != hwnd) { g_hot = -1; g_hotTab = -1; g_hotClose = false; ensureAnim(); }
        return 0;
    case WM_LBUTTONDOWN: onLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), false); return 0;
    case WM_LBUTTONDBLCLK: onLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), true); return 0;
    case WM_LBUTTONUP: onLButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)l != hwnd) { g_pressBtn = -1; g_pressClose = nullptr; g_dragIdx = -1; g_dragging = false; }
        return 0;
    case WM_MBUTTONUP: {
        Hit h = hitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (h.kind == H_TAB || h.kind == H_TABCLOSE) closeDoc(h.idx);
        return 0;
    }
    case WM_TIMER:
        if (w == T_ANIM) tickAnim();
        else if (w == T_TICK) onTick();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW(drop, i, buf, (UINT)_countof(buf))) openFile(buf);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        saveAllPending();
        if (!g_docs.empty()) saveSession();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, T_TICK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int nShow) {
    g_inst = hi;
    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    LoadLibraryW(L"Msftedit.dll");
    enableDarkMenus();

    GdiplusStartupInput gsi;
    ULONG_PTR gdiTok = 0;
    GdiplusStartup(&gdiTok, &gsi, nullptr);

    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        g_dataDir = std::wstring(appData) + L"\\Editor";
        CoTaskMemFree(appData);
    } else {
        g_dataDir = L".";
    }
    CreateDirectoryW(g_dataDir.c_str(), nullptr);

    g_dpi = GetDpiForSystem();
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = CreateSolidBrush(cStrip);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, kClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1100), S(720), nullptr, nullptr, hi, nullptr);
    if (!hwnd) return 1;

    restoreSession();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) openFile(argv[i]);
    if (argv) LocalFree(argv);
    if (g_docs.empty()) newDoc();
    layout();

    ShowWindow(hwnd, g_startMax ? SW_SHOWMAXIMIZED : (g_placed ? SW_SHOWNORMAL : nShow));
    UpdateWindow(hwnd);

    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_NEW }, { FVIRTKEY | FCONTROL, 'T', IDM_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_OPEN }, { FVIRTKEY | FCONTROL, 'S', IDM_SAVE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_SAVEAS },
        { FVIRTKEY | FCONTROL, 'W', IDM_CLOSETAB }, { FVIRTKEY | FCONTROL, VK_F4, IDM_CLOSETAB },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_NEXTTAB }, { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, IDM_PREVTAB },
        { FVIRTKEY | FCONTROL, 'F', IDM_FIND }, { FVIRTKEY | FCONTROL, 'H', IDM_REPLACE },
        { FVIRTKEY, VK_F3, IDM_FINDNEXT }, { FVIRTKEY | FSHIFT, VK_F3, IDM_FINDPREV },
        { FVIRTKEY, VK_ESCAPE, IDM_CLOSEFIND }, { FVIRTKEY, VK_F5, IDM_DATETIME },
        { FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_ZOOMIN }, { FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, IDM_ZOOMIN },
        { FVIRTKEY | FCONTROL, VK_ADD, IDM_ZOOMIN },
        { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_ZOOMOUT }, { FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_ZOOMOUT },
        { FVIRTKEY | FCONTROL, '0', IDM_ZOOMRESET }, { FVIRTKEY | FCONTROL, VK_NUMPAD0, IDM_ZOOMRESET },
        { FVIRTKEY | FALT, 'Z', IDM_WRAP },
        { FVIRTKEY | FCONTROL, '1', IDM_TAB1 }, { FVIRTKEY | FCONTROL, '2', IDM_TAB1 + 1 },
        { FVIRTKEY | FCONTROL, '3', IDM_TAB1 + 2 }, { FVIRTKEY | FCONTROL, '4', IDM_TAB1 + 3 },
        { FVIRTKEY | FCONTROL, '5', IDM_TAB1 + 4 }, { FVIRTKEY | FCONTROL, '6', IDM_TAB1 + 5 },
        { FVIRTKEY | FCONTROL, '7', IDM_TAB1 + 6 }, { FVIRTKEY | FCONTROL, '8', IDM_TAB1 + 7 },
        { FVIRTKEY | FCONTROL, '9', IDM_TAB1 + 8 },
    };
    HACCEL accel = CreateAcceleratorTableW(acc, (int)_countof(acc));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(hwnd, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyAcceleratorTable(accel);
    g_uiFont.reset(); g_smallFont.reset(); g_fLeft.reset(); g_fCenter.reset(); g_fRight.reset();
    GdiplusShutdown(gdiTok);
    OleUninitialize();
    return (int)msg.wParam;
}
