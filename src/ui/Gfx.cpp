#include "ui/Gfx.h"

#include <algorithm>
#include <cmath>

namespace pt {

namespace {
using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

struct FontSpec {
    float size;
    DWRITE_FONT_WEIGHT weight;
    bool icon;
};
const FontSpec kFonts[] = {
    {12.f, DWRITE_FONT_WEIGHT_NORMAL, false},    // Small
    {12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false}, // SmallBold
    {13.5f, DWRITE_FONT_WEIGHT_NORMAL, false},   // Body
    {13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false},// BodyBold
    {15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false}, // Section
    {20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false}, // Title
    {30.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false}, // Display
    {16.f, DWRITE_FONT_WEIGHT_NORMAL, true},     // Icon
    {12.f, DWRITE_FONT_WEIGHT_NORMAL, true},     // IconSmall
    {22.f, DWRITE_FONT_WEIGHT_NORMAL, true},     // IconLarge
};

bool fontInstalled(IDWriteFactory* f, const wchar_t* family) {
    Com<IDWriteFontCollection> fonts;
    if (FAILED(f->GetSystemFontCollection(fonts.put(), FALSE))) return false;
    UINT32 index;
    BOOL exists = FALSE;
    fonts->FindFamilyName(family, &index, &exists);
    return exists != FALSE;
}

const wchar_t* gIconFamily = L"Segoe MDL2 Assets";
}  // namespace

const wchar_t* Gfx::iconFontFamily() { return gIconFamily; }

Gfx::~Gfx() {
    discardDevice();
    for (auto& e : ellipsis_) e.reset();
    for (auto& f : formats_) f.reset();
    wic_.reset();
    dwrite_.reset();
    factory_.reset();
    if (d2dLib_) FreeLibrary(d2dLib_);
    if (dwLib_) FreeLibrary(dwLib_);
}

bool Gfx::init() {
    d2dLib_ = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    dwLib_ = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d2dLib_ || !dwLib_) return false;
    auto d2dCreate = reinterpret_cast<D2D1CreateFactoryFn>(GetProcAddress(d2dLib_, "D2D1CreateFactory"));
    auto dwCreate = reinterpret_cast<DWriteCreateFactoryFn>(GetProcAddress(dwLib_, "DWriteCreateFactory"));
    if (!d2dCreate || !dwCreate) return false;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(d2dCreate(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts, (void**)factory_.put())))
        return false;
    if (FAILED(dwCreate(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dwrite_.put())))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                     (void**)wic_.put());
    gIconFamily = fontInstalled(dwrite_.get(), L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    const wchar_t* textFamily = fontInstalled(dwrite_.get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                                                         : L"Segoe UI";
    for (int i = 0; i < 10; ++i) {
        auto& spec = kFonts[i];
        if (FAILED(dwrite_->CreateTextFormat(spec.icon ? gIconFamily : textFamily, nullptr, spec.weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec.size, L"",
                                             formats_[i].put())))
            return false;
        dwrite_->CreateEllipsisTrimmingSign(formats_[i].get(), ellipsis_[i].put());
    }
    return true;
}

void Gfx::setDpi(float dpi) {
    dpi_ = dpi;
    if (rt_) rt_->SetDpi(dpi, dpi);
}

IDWriteTextFormat* Gfx::format(Font f) { return formats_[(int)f].get(); }

bool Gfx::begin(HWND hwnd) {
    hwnd_ = hwnd;
    if (!rt_) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                  dpi_, dpi_);
        auto hprops = D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)));
        if (FAILED(factory_->CreateHwndRenderTarget(props, hprops, rt_.put()))) return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(theme::text, brush_.put());
    }
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::IdentityMatrix());
    return true;
}

void Gfx::end() {
    if (!rt_) return;
    HRESULT hr = rt_->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) discardDevice();
}

void Gfx::discardDevice() {
    images_.clear();
    brush_.reset();
    rt_.reset();
}

void Gfx::resize(UINT w, UINT h) {
    if (rt_) rt_->Resize(D2D1::SizeU(std::max<UINT>(1, w), std::max<UINT>(1, h)));
}

void Gfx::clear(Color c) { rt_->Clear(c); }

void Gfx::fill(const D2D1_RECT_F& r, Color c) {
    brush_->SetColor(c);
    rt_->FillRectangle(r, brush_.get());
}

void Gfx::round(const D2D1_RECT_F& r, float radius, Color c) {
    brush_->SetColor(c);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get());
}

void Gfx::roundStroke(const D2D1_RECT_F& r, float radius, Color c, float width) {
    brush_->SetColor(c);
    D2D1_RECT_F in{r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2};
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), brush_.get(), width);
}

void Gfx::line(float x1, float y1, float x2, float y2, Color c, float width) {
    brush_->SetColor(c);
    rt_->DrawLine({x1, y1}, {x2, y2}, brush_.get(), width);
}

void Gfx::circle(float cx, float cy, float radius, Color c) {
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse({cx, cy}, radius, radius), brush_.get());
}

void Gfx::text(std::wstring_view s, const D2D1_RECT_F& r, Font f, Color c, Align a, bool wrap) {
    if (s.empty() || r.right <= r.left) return;
    auto* fmt = format(f);
    fmt->SetTextAlignment(a == Align::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                               : DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fmt->SetTrimming(&trim, ellipsis_[(int)f].get());
    brush_->SetColor(c);
    rt_->DrawText(s.data(), (UINT32)s.size(), fmt, r, brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::textWidth(std::wstring_view s, Font f) {
    if (s.empty()) return 0;
    auto* fmt = format(f);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Com<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.data(), (UINT32)s.size(), fmt, 10000.f, 100.f, layout.put()))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

Gfx::Image* Gfx::loadImage(const std::wstring& key) {
    auto it = images_.find(key);
    if (it != images_.end()) return it->second.get();
    auto img = std::make_unique<Image>();
    img->failed = true;
    size_t bar = key.rfind(L'|');
    std::wstring path = key.substr(0, bar);
    UINT targetH = (UINT)_wtoi(key.c_str() + bar + 1);
    if (wic_ && rt_) {
        Com<IWICBitmapDecoder> decoder;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICBitmapScaler> scaler;
        Com<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        if (SUCCEEDED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, decoder.put())) &&
            SUCCEEDED(decoder->GetFrame(0, frame.put())) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
            IWICBitmapSource* src = frame.get();
            if (targetH && h > targetH) {
                UINT tw = std::max<UINT>(1, (UINT)((double)w * targetH / h));
                if (SUCCEEDED(wic_->CreateBitmapScaler(scaler.put())) &&
                    SUCCEEDED(scaler->Initialize(frame.get(), tw, targetH, WICBitmapInterpolationModeFant)))
                    src = scaler.get();
            }
            if (SUCCEEDED(wic_->CreateFormatConverter(conv.put())) &&
                SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(rt_->CreateBitmapFromWicBitmap(conv.get(), nullptr, img->bitmap.put())) &&
                SUCCEEDED(rt_->CreateBitmapBrush(img->bitmap.get(), img->brush.put())))
                img->failed = false;
        }
    }
    auto* raw = img.get();
    images_.emplace(key, std::move(img));
    return raw;
}

bool Gfx::image(const std::wstring& path, const D2D1_RECT_F& r, float radius) {
    if (path.empty() || !rt_) return false;
    float rw = r.right - r.left, rh = r.bottom - r.top;
    UINT targetPx = (UINT)std::ceil(rh * dpiScale());
    UINT bucket = targetPx <= 128 ? 128 : targetPx <= 256 ? 256 : 512;
    auto* img = loadImage(path + L"|" + std::to_wstring(bucket));
    if (!img || img->failed) return false;
    auto size = img->bitmap->GetSize();  // DIPs
    if (size.width <= 0 || size.height <= 0) return false;
    float s = std::max(rw / size.width, rh / size.height);
    float ox = r.left + (rw - size.width * s) / 2, oy = r.top + (rh - size.height * s) / 2;
    img->brush->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(ox, oy));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), img->brush.get());
    return true;
}

void Gfx::pushClip(const D2D1_RECT_F& r) { rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::popClip() { rt_->PopAxisAlignedClip(); }

void Gfx::setTranslation(float dx, float dy) { rt_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy)); }

}  // namespace pt
