#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pt {

// Small dark-themed modal form built from native controls (edit boxes need real text input).
class Form {
public:
    explicit Form(std::wstring title, int widthDip = 460);

    int label(const std::wstring& text, bool muted = true);
    int edit(const std::wstring& label, const std::wstring& value, int lines = 1, const std::wstring& cue = {});
    // Two short edits on one row (e.g. hours + minutes). Returns the first id; the second is id + 1.
    int editPair(const std::wstring& label, const std::wstring& a, const std::wstring& aCue, const std::wstring& b,
                 const std::wstring& bCue);
    int combo(const std::wstring& label, const std::vector<std::wstring>& items, int selected);
    int check(const std::wstring& label, bool checked);
    // Adds a "Browse…" button beside edit `editId`; picker returns the new text.
    void browse(int editId, std::function<std::optional<std::wstring>(HWND)> picker);
    void setOkLabel(std::wstring s) { okLabel_ = std::move(s); }
    // Returns an error message to keep the form open, or empty to accept.
    std::function<std::wstring(Form&)> validate;

    bool show(HWND owner);

    std::wstring text(int id) const;
    int selection(int id) const;
    bool checked(int id) const;

    // Internal
    struct Item {
        enum Kind { Label, Edit, Pair, Combo, Check } kind;
        std::wstring label, value, value2, cue, cue2;
        bool muted = false;
        int lines = 1;
        std::vector<std::wstring> items;
        int sel = 0;
        bool on = false;
        std::function<std::optional<std::wstring>(HWND)> picker;
        HWND ctl = nullptr, ctl2 = nullptr, browseBtn = nullptr;
    };
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void build(HWND hwnd);
    void capture();
    std::wstring title_;
    std::wstring okLabel_ = L"Save";
    int width_;
    std::vector<Item> items_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH bgBrush_ = nullptr, editBrush_ = nullptr;
    int result_ = 0;  // 0 running, 1 ok, 2 cancel
    float scale_ = 1.f;
};

void enableDarkMenus();
void applyDarkTitleBar(HWND hwnd);
void showError(HWND owner, const std::wstring& message);
void showInfo(HWND owner, const std::wstring& title, const std::wstring& message);
bool confirm(HWND owner, const std::wstring& title, const std::wstring& message);

std::optional<std::wstring> openFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                                           const std::wstring& initialDir = {});
std::optional<std::wstring> saveFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                                           const std::wstring& defaultName, const wchar_t* defaultExt);
std::optional<std::wstring> pickFolderDialog(HWND owner, const wchar_t* title);

// Parses "1234.5" or "1234" hours and/or minutes fields into seconds. nullopt on invalid input.
std::optional<int64_t> parseHoursMinutes(const std::wstring& hours, const std::wstring& minutes);

}  // namespace pt
