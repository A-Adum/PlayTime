#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pt {

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** put() {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

using Color = D2D1_COLOR_F;
constexpr Color rgb(unsigned v, float a = 1.f) {
    return {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, a};
}
constexpr Color withAlpha(Color c, float a) { return {c.r, c.g, c.b, a}; }

namespace theme {
inline constexpr Color sidebar = rgb(0x141619);
inline constexpr Color bg = rgb(0x1A1C20);
inline constexpr Color card = rgb(0x222529);
inline constexpr Color cardHover = rgb(0x2A2D32);
inline constexpr Color raised = rgb(0x2C3036);
inline constexpr Color border = rgb(0x30343A);
inline constexpr Color text = rgb(0xE8EAED);
inline constexpr Color muted = rgb(0x9BA1A9);
inline constexpr Color faint = rgb(0x6B717A);
inline constexpr Color accent = rgb(0x5B9BF0);
inline constexpr Color accentDim = rgb(0x3A6FB5);
inline constexpr Color success = rgb(0x4CC38A);
inline constexpr Color warning = rgb(0xE5B454);
inline constexpr Color danger = rgb(0xE5636B);
}  // namespace theme

enum class Font { Small, SmallBold, Body, BodyBold, Section, Title, Display, Icon, IconSmall, IconLarge };
enum class Align { Left, Center, Right };

inline D2D1_RECT_F rect(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
inline bool contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Thin Direct2D/DirectWrite/WIC wrapper. d2d1.dll and dwrite.dll are loaded on demand and unloaded when
// the window closes, so the tray-only process carries no rendering libraries.
class Gfx {
public:
    Gfx() = default;
    ~Gfx();
    bool init();
    void setDpi(float dpi);
    float dpiScale() const { return dpi_ / 96.f; }

    bool begin(HWND hwnd);
    void end();  // presents; discards device resources on D2DERR_RECREATE_TARGET
    void resize(UINT w, UINT h);

    void clear(Color c);
    void fill(const D2D1_RECT_F& r, Color c);
    void round(const D2D1_RECT_F& r, float radius, Color c);
    void roundStroke(const D2D1_RECT_F& r, float radius, Color c, float width = 1.f);
    void line(float x1, float y1, float x2, float y2, Color c, float width = 1.f);
    void circle(float cx, float cy, float radius, Color c);
    void text(std::wstring_view s, const D2D1_RECT_F& r, Font f, Color c, Align a = Align::Left, bool wrap = false);
    float textWidth(std::wstring_view s, Font f);
    // Draws an image cropped to fill `r` with rounded corners. Returns false if it could not be loaded.
    bool image(const std::wstring& path, const D2D1_RECT_F& r, float radius);
    void pushClip(const D2D1_RECT_F& r);
    void popClip();
    void setTranslation(float dx, float dy);
    static const wchar_t* iconFontFamily();

private:
    IDWriteTextFormat* format(Font f);
    struct Image {
        Com<ID2D1Bitmap> bitmap;
        Com<ID2D1BitmapBrush> brush;
        bool failed = false;
    };
    Image* loadImage(const std::wstring& path);
    void discardDevice();

    HMODULE d2dLib_ = nullptr, dwLib_ = nullptr;
    Com<ID2D1Factory> factory_;
    Com<IDWriteFactory> dwrite_;
    Com<IWICImagingFactory> wic_;
    Com<ID2D1HwndRenderTarget> rt_;
    Com<ID2D1SolidColorBrush> brush_;
    Com<IDWriteTextFormat> formats_[10];
    Com<IDWriteInlineObject> ellipsis_[10];
    std::unordered_map<std::wstring, std::unique_ptr<Image>> images_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96.f;
};

}  // namespace pt
