#include "ui/Dialogs.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <cmath>
#include <cwchar>

#include "util/Str.h"

namespace pt {

namespace {
constexpr const wchar_t* kFormClass = L"PlayTime.Form";
constexpr int kIdOk = IDOK, kIdCancel = IDCANCEL, kIdBase = 1000, kIdBrowseBase = 2000;
const COLORREF kBg = RGB(0x1F, 0x22, 0x26), kText = RGB(0xE8, 0xEA, 0xED), kMuted = RGB(0x9B, 0xA1, 0xA9),
               kEditBg = RGB(0x2A, 0x2D, 0x32);

bool isMultilineEdit(HWND h) {
    wchar_t cls[16];
    GetClassNameW(h, cls, 16);
    return _wcsicmp(cls, L"Edit") == 0 && (GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE);
}

std::wstring windowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n, L'\0');
    if (n) GetWindowTextW(h, s.data(), n + 1);
    return s;
}
}  // namespace

void enableDarkMenus() {
    // uxtheme ordinals 135 (SetPreferredAppMode) and 136 (FlushMenuThemes): dark popup menus on Windows 10 1903+.
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetModeFn = int(WINAPI*)(int);
    using FlushFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    OSVERSIONINFOEXW v{sizeof v};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (rtl) rtl(&v);
    if (setMode && flush && v.dwBuildNumber >= 18362) {
        setMode(2);  // ForceDark
        flush();
    }
}

void applyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark)))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof dark);  // pre-20H1 value
}

void showError(HWND owner, const std::wstring& message) {
    MessageBoxW(owner, message.c_str(), L"PlayTime", MB_OK | MB_ICONERROR);
}

void showInfo(HWND owner, const std::wstring& title, const std::wstring& message) {
    MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

bool confirm(HWND owner, const std::wstring& title, const std::wstring& message) {
    return MessageBoxW(owner, message.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

std::optional<std::wstring> openFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                                           const std::wstring& initialDir) {
    wchar_t buf[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)std::size(buf);
    ofn.lpstrTitle = title;
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return std::wstring(buf);
}

std::optional<std::wstring> saveFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                                           const std::wstring& defaultName, const wchar_t* defaultExt) {
    wchar_t buf[MAX_PATH * 4];
    wcsncpy_s(buf, defaultName.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)std::size(buf);
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return std::wstring(buf);
}

std::optional<std::wstring> pickFolderDialog(HWND owner, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)))
        return std::nullopt;
    std::optional<std::wstring> out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

namespace {
// Accepts "3500", "12.5", "12,5" (decimal comma) and "4,250" / "4,250.5" (thousands grouping).
std::optional<double> parseAmount(const std::wstring& text) {
    std::wstring s = trim(text);
    if (s.empty()) return 0.0;
    size_t dot = s.find(L'.');
    auto groups = split(s.substr(0, dot), L',');
    bool grouping = groups.size() > 1 && !groups[0].empty() && groups[0].size() <= 3;
    for (size_t i = 1; grouping && i < groups.size(); ++i)
        if (groups[i].size() != 3) grouping = false;
    std::wstring clean;
    for (wchar_t c : s) {
        if (c == L',') {
            if (grouping) continue;
            if (dot != std::wstring::npos) return std::nullopt;
            c = L'.';
        }
        clean += c;
    }
    wchar_t* end = nullptr;
    double v = wcstod(clean.c_str(), &end);
    if (!end || *end != 0 || !std::isfinite(v) || v < 0) return std::nullopt;
    return v;
}
}  // namespace

std::optional<int64_t> parseHoursMinutes(const std::wstring& hours, const std::wstring& minutes) {
    auto h = parseAmount(hours), m = parseAmount(minutes);
    if (!h || !m || *h > 1e6 || *m > 1e7) return std::nullopt;
    return (int64_t)std::llround(*h * 3600.0 + *m * 60.0);
}

// ---------------------------------------------------------------- Form

Form::Form(std::wstring title, int widthDip) : title_(std::move(title)), width_(widthDip) {}

int Form::label(const std::wstring& text, bool muted) {
    Item it{Item::Label};
    it.label = text;
    it.muted = muted;
    items_.push_back(std::move(it));
    return (int)items_.size() - 1;
}

int Form::edit(const std::wstring& label, const std::wstring& value, int lines, const std::wstring& cue) {
    Item it{Item::Edit};
    it.label = label;
    it.value = value;
    it.lines = lines;
    it.cue = cue;
    items_.push_back(std::move(it));
    return (int)items_.size() - 1;
}

int Form::editPair(const std::wstring& label, const std::wstring& a, const std::wstring& aCue, const std::wstring& b,
                   const std::wstring& bCue) {
    Item it{Item::Pair};
    it.label = label;
    it.value = a;
    it.cue = aCue;
    it.value2 = b;
    it.cue2 = bCue;
    items_.push_back(std::move(it));
    int id = (int)items_.size() - 1;
    items_.push_back(Item{Item::Label});  // placeholder so id + 1 addresses the second field
    items_.back().label = L"\x01";
    return id;
}

int Form::combo(const std::wstring& label, const std::vector<std::wstring>& items, int selected) {
    Item it{Item::Combo};
    it.label = label;
    it.items = items;
    it.sel = selected;
    items_.push_back(std::move(it));
    return (int)items_.size() - 1;
}

int Form::check(const std::wstring& label, bool checked) {
    Item it{Item::Check};
    it.label = label;
    it.on = checked;
    items_.push_back(std::move(it));
    return (int)items_.size() - 1;
}

void Form::browse(int editId, std::function<std::optional<std::wstring>(HWND)> picker) {
    items_[(size_t)editId].picker = std::move(picker);
}

std::wstring Form::text(int id) const {
    if (id > 0 && items_[(size_t)id].kind == Item::Label && items_[(size_t)id].label == L"\x01") {
        auto& pair = items_[(size_t)id - 1];
        return pair.ctl2 && hwnd_ ? windowText(pair.ctl2) : pair.value2;
    }
    auto& it = items_[(size_t)id];
    if (it.ctl && hwnd_) return windowText(it.ctl);
    return it.value;
}

int Form::selection(int id) const {
    auto& it = items_[(size_t)id];
    if (it.ctl && hwnd_) return (int)SendMessageW(it.ctl, CB_GETCURSEL, 0, 0);
    return it.sel;
}

bool Form::checked(int id) const {
    auto& it = items_[(size_t)id];
    if (it.ctl && hwnd_) return SendMessageW(it.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return it.on;
}

void Form::capture() {
    for (auto& it : items_) {
        if (!it.ctl) continue;
        switch (it.kind) {
            case Item::Edit: it.value = windowText(it.ctl); break;
            case Item::Pair:
                it.value = windowText(it.ctl);
                it.value2 = windowText(it.ctl2);
                break;
            case Item::Combo: it.sel = (int)SendMessageW(it.ctl, CB_GETCURSEL, 0, 0); break;
            case Item::Check: it.on = SendMessageW(it.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED; break;
            default: break;
        }
    }
}

void Form::build(HWND hwnd) {
    auto px = [&](float dip) { return (int)std::lround(dip * scale_); };
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    const int margin = px(20), inner = px((float)width_) - margin * 2;
    int y = px(16);
    auto make = [&](const wchar_t* cls, const std::wstring& text, DWORD style, int x, int yy, int w, int h, int id,
                    DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, yy, w, h, hwnd,
                                 (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font_, FALSE);
        return c;
    };
    auto themed = [](HWND c, const wchar_t* theme) { SetWindowTheme(c, theme, nullptr); };
    // Measure wrapped label height.
    HDC dc = GetDC(hwnd);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    auto textHeight = [&](const std::wstring& s, int w) {
        RECT r{0, 0, w, 0};
        DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        return (int)(r.bottom - r.top);
    };
    const int lineH = px(18), editH = px(28);
    for (size_t i = 0; i < items_.size(); ++i) {
        auto& it = items_[i];
        int id = kIdBase + (int)i;
        switch (it.kind) {
            case Item::Label: {
                if (it.label == L"\x01") break;
                int h = textHeight(it.label, inner);
                it.ctl = make(L"STATIC", it.label, SS_NOPREFIX, margin, y, inner, h, id);
                SetWindowLongPtrW(it.ctl, GWLP_USERDATA, it.muted ? 1 : 0);
                y += h + px(10);
                break;
            }
            case Item::Edit:
            case Item::Pair: {
                make(L"STATIC", it.label, SS_NOPREFIX, margin, y, inner, lineH, 0);
                y += lineH + px(4);
                if (it.kind == Item::Pair) {
                    int w = px(110), unitW = px(64);
                    it.ctl = make(L"EDIT", it.value, WS_TABSTOP | ES_AUTOHSCROLL, margin, y, w, editH, id, WS_EX_CLIENTEDGE);
                    HWND u1 = make(L"STATIC", it.cue, SS_NOPREFIX | SS_CENTERIMAGE, margin + w + px(8), y, unitW, editH, 0);
                    int x2 = margin + w + px(8) + unitW;
                    it.ctl2 = make(L"EDIT", it.value2, WS_TABSTOP | ES_AUTOHSCROLL, x2, y, w, editH, id + 1,
                                   WS_EX_CLIENTEDGE);
                    HWND u2 = make(L"STATIC", it.cue2, SS_NOPREFIX | SS_CENTERIMAGE, x2 + w + px(8), y, unitW, editH, 0);
                    SetWindowLongPtrW(u1, GWLP_USERDATA, 1);
                    SetWindowLongPtrW(u2, GWLP_USERDATA, 1);
                    SendMessageW(it.ctl, EM_SETCUEBANNER, TRUE, (LPARAM)it.cue.c_str());
                    SendMessageW(it.ctl2, EM_SETCUEBANNER, TRUE, (LPARAM)it.cue2.c_str());
                    themed(it.ctl, L"DarkMode_CFD");
                    themed(it.ctl2, L"DarkMode_CFD");
                    y += editH + px(12);
                    break;
                }
                bool multi = it.lines > 1;
                int h = multi ? px(8) + it.lines * px(18) : editH;
                int w = it.picker ? inner - px(96) : inner;
                DWORD style = WS_TABSTOP | (multi ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL) : ES_AUTOHSCROLL);
                std::wstring v = it.value;
                // Multiline edits need CRLF.
                if (multi) {
                    std::wstring t;
                    for (size_t k = 0; k < v.size(); ++k) {
                        if (v[k] == L'\n' && (k == 0 || v[k - 1] != L'\r')) t += L'\r';
                        t += v[k];
                    }
                    v = t;
                }
                it.ctl = make(L"EDIT", v, style, margin, y, w, h, id, WS_EX_CLIENTEDGE);
                if (!it.cue.empty()) SendMessageW(it.ctl, EM_SETCUEBANNER, TRUE, (LPARAM)it.cue.c_str());
                themed(it.ctl, multi ? L"DarkMode_Explorer" : L"DarkMode_CFD");
                if (it.picker) {
                    it.browseBtn = make(L"BUTTON", L"Browse\u2026", WS_TABSTOP | BS_PUSHBUTTON, margin + w + px(8), y,
                                        px(88), editH, kIdBrowseBase + (int)i);
                    themed(it.browseBtn, L"DarkMode_Explorer");
                }
                y += h + px(12);
                break;
            }
            case Item::Combo: {
                make(L"STATIC", it.label, SS_NOPREFIX, margin, y, inner, lineH, 0);
                y += lineH + px(4);
                it.ctl = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, margin, y, inner, px(300), id);
                for (auto& s : it.items) SendMessageW(it.ctl, CB_ADDSTRING, 0, (LPARAM)s.c_str());
                SendMessageW(it.ctl, CB_SETCURSEL, (WPARAM)it.sel, 0);
                themed(it.ctl, L"DarkMode_CFD");
                y += editH + px(12);
                break;
            }
            case Item::Check: {
                it.ctl = make(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, margin, y + px(2), px(18), px(18), id);
                SendMessageW(it.ctl, BM_SETCHECK, it.on ? BST_CHECKED : BST_UNCHECKED, 0);
                // Text in a separate static: themed checkbox text ignores WM_CTLCOLORBTN.
                make(L"STATIC", it.label, SS_NOPREFIX | SS_NOTIFY, margin + px(26), y + px(1), inner - px(26), lineH, id + 5000);
                y += lineH + px(14);
                break;
            }
        }
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);

    y += px(6);
    int bw = px(96), bh = px(32);
    HWND ok = make(L"BUTTON", okLabel_, WS_TABSTOP | BS_DEFPUSHBUTTON, margin + inner - bw * 2 - px(10), y, bw, bh, kIdOk);
    HWND cancel = make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, margin + inner - bw, y, bw, bh, kIdCancel);
    themed(ok, L"DarkMode_Explorer");
    themed(cancel, L"DarkMode_Explorer");
    y += bh + px(18);

    RECT rc{0, 0, px((float)width_), y};
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, (UINT)std::lround(scale_ * 96));
    HWND owner = GetWindow(hwnd, GW_OWNER);
    RECT orc;
    if (owner && IsWindowVisible(owner)) GetWindowRect(owner, &orc);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &orc, 0);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(hwnd, nullptr, orc.left + (orc.right - orc.left - w) / 2, orc.top + (orc.bottom - orc.top - h) / 3, w, h,
                 SWP_NOZORDER);
    for (auto& it : items_)
        if (it.ctl && it.kind != Item::Label) {
            SetFocus(it.ctl);
            if (it.kind == Item::Edit || it.kind == Item::Pair) SendMessageW(it.ctl, EM_SETSEL, 0, -1);
            break;
        }
}

static LRESULT CALLBACK formProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    }
    auto* f = reinterpret_cast<Form*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return f ? f->handle(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Form::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            bool muted = GetWindowLongPtrW((HWND)lp, GWLP_USERDATA) == 1;
            SetTextColor(dc, muted ? kMuted : kText);
            SetBkColor(dc, kBg);
            return (LRESULT)bgBrush_;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, kText);
            SetBkColor(dc, kEditBg);
            return (LRESULT)editBrush_;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, bgBrush_);
            return 1;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == kIdOk) {
                if (validate) {
                    auto err = validate(*this);
                    if (!err.empty()) {
                        MessageBoxW(hwnd, err.c_str(), title_.c_str(), MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                }
                capture();
                result_ = 1;
                return 0;
            }
            if (id == kIdCancel) {
                result_ = 2;
                return 0;
            }
            if (id >= kIdBrowseBase && id < kIdBrowseBase + (int)items_.size()) {
                auto& it = items_[(size_t)(id - kIdBrowseBase)];
                if (it.picker)
                    if (auto v = it.picker(hwnd)) SetWindowTextW(it.ctl, v->c_str());
                return 0;
            }
            if (id >= kIdBase + 5000 && HIWORD(wp) == STN_CLICKED) {  // clicking a checkbox label toggles it
                auto& it = items_[(size_t)(id - kIdBase - 5000)];
                if (it.ctl) SendMessageW(it.ctl, BM_SETCHECK, SendMessageW(it.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            result_ = 2;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Form::show(HWND owner) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = formProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)GetClassLongPtrW(owner, GCLP_HICON);
        wc.lpszClassName = kFormClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    scale_ = dpi / 96.f;
    font_ = CreateFontW(-MulDiv(10, (int)dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                        0, L"Segoe UI");
    bgBrush_ = CreateSolidBrush(kBg);
    editBrush_ = CreateSolidBrush(kEditBg);
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kFormClass, title_.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0,
                            100, 100, owner, nullptr, inst, this);
    if (!hwnd_) return false;
    applyDarkTitleBar(hwnd_);
    build(hwnd_);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd_, SW_SHOW);

    MSG msg;
    bool quit = false;
    while (result_ == 0) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) {
            quit = true;
            result_ = 2;
            break;
        }
        if (msg.message == WM_KEYDOWN && (msg.hwnd == hwnd_ || IsChild(hwnd_, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) {
                result_ = 2;
                continue;
            }
            if (msg.wParam == VK_RETURN && !isMultilineEdit(msg.hwnd)) {
                wchar_t cls[16];
                GetClassNameW(msg.hwnd, cls, 16);
                if (_wcsicmp(cls, L"Button") != 0) {
                    SendMessageW(hwnd_, WM_COMMAND, kIdOk, 0);
                    continue;
                }
            }
        }
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (result_ != 1) capture();  // keep text() usable for callers either way
    if (owner) EnableWindow(owner, TRUE);
    HWND h = hwnd_;
    hwnd_ = nullptr;  // accessors now return captured values
    DestroyWindow(h);
    if (owner) SetForegroundWindow(owner);
    DeleteObject(font_);
    DeleteObject(bgBrush_);
    DeleteObject(editBrush_);
    if (quit) PostQuitMessage(0);
    return result_ == 1;
}

}  // namespace pt
