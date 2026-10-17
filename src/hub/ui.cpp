#include "hub/ui.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

#include "hub/analysis.h"
#include "hub/asio_driver.h"

using Microsoft::WRL::ComPtr;

namespace {

// Palette from the mockup.
constexpr UINT32 kBg = 0x0E0F11;
constexpr UINT32 kPanel = 0x17191C;
constexpr UINT32 kBorder = 0x26292E;
constexpr UINT32 kTile = 0x121417;
constexpr UINT32 kText = 0xECEAE4;
constexpr UINT32 kSecondary = 0xA3A19A;
constexpr UINT32 kTertiary = 0x8A8880;
constexpr UINT32 kAccent = 0x3CCFB4;
constexpr UINT32 kAmber = 0xF0B23C;
constexpr UINT32 kRed = 0xF2584B;
constexpr UINT32 kTrack = 0x0B0C0E;
constexpr UINT32 kControl = 0x33373D;
constexpr UINT32 kHover = 0x22252A;

constexpr float kPad = 24.0f;
constexpr float kGap = 16.0f;
constexpr float kRightWidth = 340.0f;  // the side column
static_assert(kRightWidth + kGap == Ui::kSideWidth);
constexpr float kMeterRangeDb = 60.0f;
constexpr float kMeterWidth = 64.0f;
constexpr float kBarWidth = 22.0f;
constexpr float kDawHeight = 302.0f;

const wchar_t* const kTileLabels[4] = {L"Clock lock", L"Dropouts", L"Packet gaps", L"Playback stream"};

D2D1_RECT_F Rect(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

std::wstring PickFamily(IDWriteFactory* dwrite, std::initializer_list<const wchar_t*> candidates) {
  ComPtr<IDWriteFontCollection> fonts;
  dwrite->GetSystemFontCollection(&fonts);
  for (const wchar_t* name : candidates) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) return name;
  }
  return L"Segoe UI";
}

}  // namespace

HRESULT Ui::Init(HWND hwnd) {
  hwnd_ = hwnd;
  dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
  if (SUCCEEDED(hr)) {
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
  }
  if (SUCCEEDED(hr)) hr = CreateFormats();
  return hr;
}

ComPtr<IDWriteTextFormat> Ui::MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
  ComPtr<IDWriteTextFormat> format;
  dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                            L"en-us", &format);
  if (format) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  return format;
}

HRESULT Ui::CreateFormats() {
  std::wstring text = PickFamily(dwrite_.Get(), {L"Segoe UI Variable Text", L"Segoe UI"});
  std::wstring display = PickFamily(dwrite_.Get(), {L"Segoe UI Variable Display", L"Segoe UI"});
  std::wstring mono = PickFamily(dwrite_.Get(), {L"Cascadia Mono", L"Consolas"});
  title_ = MakeFormat(display.c_str(), 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  heading_ = MakeFormat(display.c_str(), 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  body_ = MakeFormat(text.c_str(), 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
  small_ = MakeFormat(text.c_str(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
  caps_ = MakeFormat(text.c_str(), 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  mono_ = MakeFormat(mono.c_str(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
  mono_large_ = MakeFormat(mono.c_str(), 17.0f, DWRITE_FONT_WEIGHT_NORMAL);
  trimmed_ = MakeFormat(text.c_str(), 13.0f, DWRITE_FONT_WEIGHT_NORMAL);
  small_trimmed_ = MakeFormat(text.c_str(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
  if (!title_ || !heading_ || !body_ || !small_ || !caps_ || !mono_ || !mono_large_ || !trimmed_ || !small_trimmed_) {
    return E_FAIL;
  }

  for (IDWriteTextFormat* single : {title_.Get(), heading_.Get(), caps_.Get(), mono_.Get(), mono_large_.Get(),
                                    trimmed_.Get(), small_trimmed_.Get()}) {
    single->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  for (IDWriteTextFormat* trimmed : {trimmed_.Get(), small_trimmed_.Get()}) {
    ComPtr<IDWriteInlineObject> ellipsis;
    dwrite_->CreateEllipsisTrimmingSign(trimmed, &ellipsis);
    trimmed->SetTrimming(&trimming, ellipsis.Get());
  }
  return S_OK;
}

HRESULT Ui::CreateTarget() {
  if (hwnd_target_) return S_OK;
  RECT client;
  GetClientRect(hwnd_, &client);
  D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      dpi_, dpi_);
  HRESULT hr = factory_->CreateHwndRenderTarget(
      properties,
      D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(client.right - client.left, client.bottom - client.top)),
      &hwnd_target_);
  if (SUCCEEDED(hr)) hr = hwnd_target_->CreateSolidColorBrush(D2D1::ColorF(kText), &brush_);
  if (SUCCEEDED(hr)) target_ = hwnd_target_;
  return hr;
}

void Ui::Resize() {
  if (!hwnd_target_) return;
  RECT client;
  GetClientRect(hwnd_, &client);
  hwnd_target_->Resize(D2D1::SizeU(client.right - client.left, client.bottom - client.top));
}

HRESULT Ui::Paint(const UiModel& model, Hit hover) {
  HRESULT hr = CreateTarget();
  if (FAILED(hr)) return hr;
  target_->BeginDraw();
  Draw(model, hover);
  hr = target_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    brush_.Reset();
    target_.Reset();
    hwnd_target_.Reset();
  }
  return hr;
}

HRESULT Ui::RenderToPng(const UiModel& model, UINT width_px, UINT height_px, const wchar_t* path) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> bitmap_target;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  if (SUCCEEDED(hr)) {
    hr = wic->CreateBitmap(width_px, height_px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
  }
  if (SUCCEEDED(hr)) {
    hr = factory_->CreateWicBitmapRenderTarget(
        bitmap.Get(),
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_,
                                     dpi_),
        &bitmap_target);
  }
  if (FAILED(hr)) return hr;

  ComPtr<ID2D1RenderTarget> saved_target = target_;
  ComPtr<ID2D1SolidColorBrush> saved_brush = brush_;
  target_ = bitmap_target;
  brush_.Reset();
  hr = target_->CreateSolidColorBrush(D2D1::ColorF(kText), &brush_);
  if (SUCCEEDED(hr)) {
    target_->BeginDraw();
    Draw(model, Hit::None);
    hr = target_->EndDraw();
  }
  target_ = saved_target;
  brush_ = saved_brush;
  if (FAILED(hr)) return hr;

  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
  if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
  if (SUCCEEDED(hr)) hr = frame->SetSize(width_px, height_px);
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  return hr;
}

void Ui::SetDpi(UINT dpi) {
  dpi_ = static_cast<float>(dpi);
  if (target_) target_->SetDpi(dpi_, dpi_);
}

void Ui::Text(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect, UINT32 color,
              DWRITE_TEXT_ALIGNMENT align) {
  format->SetTextAlignment(align);
  brush_->SetColor(D2D1::ColorF(color));
  target_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

float Ui::TextWidth(const std::wstring& text, IDWriteTextFormat* format) {
  ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 2000.0f, 100.0f,
                                       &layout))) {
    return 0.0f;
  }
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  return metrics.widthIncludingTrailingWhitespace;
}

void Ui::Fill(D2D1_RECT_F rect, UINT32 color, float radius) {
  brush_->SetColor(D2D1::ColorF(color));
  if (radius > 0.0f) {
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
  } else {
    target_->FillRectangle(rect, brush_.Get());
  }
}

void Ui::Stroke(D2D1_RECT_F rect, UINT32 color, float radius, float width) {
  brush_->SetColor(D2D1::ColorF(color));
  // Inset by half the stroke so it stays inside the rectangle.
  D2D1_RECT_F r = Rect(rect.left + width / 2, rect.top + width / 2, rect.right - width / 2, rect.bottom - width / 2);
  if (radius > 0.0f) {
    target_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get(), width);
  } else {
    target_->DrawRectangle(r, brush_.Get(), width);
  }
}

void Ui::Line(float x0, float y0, float x1, float y1, UINT32 color, float width) {
  brush_->SetColor(D2D1::ColorF(color));
  target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_.Get(), width);
}

void Ui::Circle(float cx, float cy, float r, UINT32 color) {
  brush_->SetColor(D2D1::ColorF(color));
  target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_.Get());
}

float Ui::Pill(float right, float y, const std::wstring& text, UINT32 dot, bool mono) {
  IDWriteTextFormat* format = mono ? mono_.Get() : body_.Get();
  float width = TextWidth(text, format) + 24.0f + (dot ? 16.0f : 0.0f);
  D2D1_RECT_F r = Rect(right - width, y, right, y + 30.0f);
  Fill(r, kPanel, 15.0f);
  Stroke(r, kBorder, 15.0f);
  float x = r.left + 12.0f;
  if (dot) {
    Circle(x + 4.0f, y + 15.0f, 4.0f, dot);
    x += 16.0f;
  }
  Text(text, format, Rect(x, y + 6.0f, r.right, y + 26.0f), kText);
  return r.left;
}

// A small cog: eight teeth around a disc with a hole in the button's colour.
void Ui::Gear(float cx, float cy, UINT32 color, UINT32 hole) {
  brush_->SetColor(D2D1::ColorF(color));
  for (int i = 0; i < 8; ++i) {
    const float a = i * 3.14159265f / 4.0f, c = std::cos(a), s = std::sin(a);
    target_->DrawLine(D2D1::Point2F(cx + 4.5f * c, cy + 4.5f * s), D2D1::Point2F(cx + 8.0f * c, cy + 8.0f * s),
                      brush_.Get(), 3.0f);
  }
  Circle(cx, cy, 5.8f, color);
  Circle(cx, cy, 2.4f, hole);
}

void Ui::Toggle(D2D1_RECT_F rect, bool on, bool hover) {
  // rect is the 44x24 track
  Fill(rect, on ? kAccent : (hover ? 0x3D4148 : kControl), 12.0f);
  float cx = on ? rect.right - 12.0f : rect.left + 12.0f;
  Circle(cx, (rect.top + rect.bottom) / 2.0f, 9.0f, kText);
}

void Ui::Button(D2D1_RECT_F rect, const std::wstring& text, bool hover, bool enabled, bool primary) {
  UINT32 color = kText;
  if (!enabled) {
    color = kTertiary;
    Stroke(rect, kBorder, 8.0f);
  } else if (primary) {
    Fill(rect, hover ? 0x5BDCC5 : kAccent, 8.0f);
    color = kBg;
  } else {
    if (hover) Fill(rect, kHover, 8.0f);
    Stroke(rect, kControl, 8.0f);
  }
  Text(text, body_.Get(), Rect(rect.left, rect.top + (rect.bottom - rect.top - 17.0f) / 2.0f, rect.right, rect.bottom),
       color, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Ui::Badge(float right, float top, const std::wstring& text, bool attention) {
  float width = TextWidth(text, small_.Get()) + 20.0f;
  D2D1_RECT_F badge = Rect(right - width, top, right, top + 24.0f);
  Stroke(badge, kControl, 12.0f);
  Text(text, small_.Get(), Rect(badge.left, top + 4.0f, badge.right, top + 22.0f), attention ? kAmber : kSecondary,
       DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Ui::Card(D2D1_RECT_F rect) {
  Fill(rect, kPanel, 12.0f);
  Stroke(rect, kBorder, 12.0f);
}

// One vertical meter column: clip light, peak readout, bar, label.
void Ui::Meter(float x, float top, float bar_height, const MeterView& meter, const wchar_t* label, bool live) {
  float cx = x + kMeterWidth / 2.0f;
  Fill(Rect(cx - kBarWidth / 2.0f, top, cx + kBarWidth / 2.0f, top + 5.0f), meter.clip ? kRed : kBorder, 2.0f);

  wchar_t readout[16];
  if (!live || meter.peak_db <= -kMeterRangeDb + 0.5f) {
    wcscpy_s(readout, live ? L"\x2212\x221E" : L"\x2014");
  } else {
    swprintf_s(readout, L"%.1f", meter.peak_db);
    if (readout[0] == L'-') readout[0] = L'\x2212';
  }
  Text(readout, mono_.Get(), Rect(x - 8.0f, top + 10.0f, x + kMeterWidth + 8.0f, top + 28.0f),
       meter.clip ? kRed : kSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

  float bar_top = top + 33.0f;
  float bar_bottom = bar_top + bar_height;
  D2D1_RECT_F bar = Rect(cx - kBarWidth / 2.0f, bar_top, cx + kBarWidth / 2.0f, bar_bottom);
  Fill(bar, kTrack, 4.0f);

  auto y_of = [&](float db) {
    float fraction = std::clamp((db + kMeterRangeDb) / kMeterRangeDb, 0.0f, 1.0f);
    return bar_bottom - fraction * bar_height;
  };
  if (live && meter.level_db > -kMeterRangeDb) {
    float level = y_of(meter.level_db);
    float amber = y_of(-12.0f), red = y_of(-3.0f);
    float l = bar.left + 1.0f, r = bar.right - 1.0f;
    Fill(Rect(l, std::max(level, amber), r, bar_bottom - 1.0f), kAccent);
    if (level < amber) Fill(Rect(l, std::max(level, red), r, amber), kAmber);
    if (level < red) Fill(Rect(l, level, r, red), kRed);
  }
  if (live && meter.peak_db > -kMeterRangeDb) {
    float y = y_of(meter.peak_db);
    Fill(Rect(bar.left + 1.0f, y, bar.right - 1.0f, y + 2.0f), kText);
  }
  Stroke(bar, kBorder, 4.0f);
  Text(label, small_.Get(), Rect(x - 8.0f, bar_bottom + 8.0f, x + kMeterWidth + 8.0f, bar_bottom + 26.0f), kSecondary,
       DWRITE_TEXT_ALIGNMENT_CENTER);
}


void Ui::Draw(const UiModel& m, Hit hover) {
  target_->Clear(D2D1::ColorF(kBg));
  D2D1_SIZE_F size = target_->GetSize();
  const float W = std::max(size.width, MinWidth(m.side_open));
  const float H = std::max(size.height, MinHeight(m.side_open));
  for (auto& r : hit_) r = Rect(0, 0, 0, 0);

  // ---- Header
  {
    float x = kPad, y = kPad;
    D2D1_RECT_F body = Rect(x + 3.0f, y + 15.0f, x + 37.0f, y + 37.0f);
    Stroke(body, kText, 4.0f, 1.5f);
    brush_->SetColor(D2D1::ColorF(kText));
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 12.0f, y + 26.0f), 4.0f, 4.0f), brush_.Get(), 1.5f);
    target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 24.0f, y + 26.0f), 4.0f, 4.0f), brush_.Get(), 1.5f);
    Line(x + 31.0f, y + 21.0f, x + 31.0f, y + 31.0f, kText, 1.5f);

    // The settings cog opens the side column; filled while it's open.
    D2D1_RECT_F cog = Rect(W - kPad - 30.0f, y + 12.0f, W - kPad, y + 42.0f);
    UINT32 fill = m.side_open ? kControl : hover == Hit::Settings ? kHover : kPanel;
    Fill(cog, fill, 15.0f);
    Stroke(cog, m.side_open ? kControl : kBorder, 15.0f);
    Gear((cog.left + cog.right) / 2.0f, (cog.top + cog.bottom) / 2.0f, m.side_open ? kText : kSecondary, fill);
    if (m.side_attention && !m.side_open) Circle(cog.right - 4.0f, cog.top + 4.0f, 4.0f, kAmber);
    SetHit(Hit::Settings, cog);

    float right = cog.left - 8.0f;
    if (!m.rate.empty()) right = Pill(right, y + 12.0f, m.rate, m.rate_kind == 2 ? kAmber : 0, true) - 8.0f;
    UINT32 dot = m.status_kind == 1 ? kAccent : m.status_kind == 2 ? kAmber : kTertiary;
    const float pills_left = Pill(right, y + 12.0f, m.status, dot, false);

    Text(L"Mbox 2 Mini", title_.Get(), Rect(x + 54.0f, y + 2.0f, pills_left - 16.0f, y + 32.0f), kText);
    Text(m.subtitle, trimmed_.Get(), Rect(x + 54.0f, y + 33.0f, pills_left - 16.0f, y + 52.0f), kSecondary);
  }

  const float content_top = kPad + 72.0f;
  const float bottom = H - kPad;
  if (!m.side_open) {
    DrawMain(m, hover, Rect(kPad, content_top, W - kPad, bottom));
    return;
  }
  const float right_left = W - kPad - kRightWidth;
  DrawMain(m, hover, Rect(kPad, content_top, right_left - kGap, bottom));

  // ---- Side column: firmware on top; the dropout log gets the space above the DAW and startup cards.
  const float cl = right_left, cr = W - kPad;
  constexpr float kStartupHeight = 72.0f;
  const float startup_top = bottom - kStartupHeight;
  const float daw_top = startup_top - kGap - kDawHeight;
  const float log_top = DrawFirmware(m, hover, content_top, cl, cr) + kGap;
  DrawLog(m, hover, Rect(cl, log_top, cr, std::max(log_top + 100.0f, daw_top - kGap)));
  DrawDaw(m, hover, daw_top, cl, cr);
  DrawStartup(m, hover, Rect(cl, startup_top, cr, bottom));
}

float Ui::Segmented(float x, float y, float height, const wchar_t* const* labels, int count, int selected, Hit first,
                    Hit hover, IDWriteTextFormat* format, bool right_aligned) {
  float widths[8] = {};
  float total = 6.0f;
  for (int i = 0; i < count && i < 8; ++i) {
    widths[i] = TextWidth(labels[i], format) + 24.0f;
    total += widths[i];
  }
  if (right_aligned) x -= total;
  D2D1_RECT_F track = Rect(x, y, x + total, y + height);
  Fill(track, kBg, 8.0f);
  Stroke(track, kControl, 8.0f);
  float sx = x + 3.0f;
  const float text_height = format == small_.Get() ? 16.0f : 17.0f;
  for (int i = 0; i < count && i < 8; ++i) {
    D2D1_RECT_F seg = Rect(sx, y + 3.0f, sx + widths[i], y + height - 3.0f);
    Hit h = static_cast<Hit>(static_cast<int>(first) + i);
    if (i == selected) {
      Fill(seg, kControl, 6.0f);
    } else if (hover == h) {
      Fill(seg, kHover, 6.0f);
    }
    Text(labels[i], format, Rect(seg.left, y + (height - text_height) / 2.0f - 1.0f, seg.right, y + height),
         i == selected ? kText : kSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    SetHit(h, seg);
    sx += widths[i];
  }
  return total;
}

// A line through `points`, or with fill_to >= 0, the area between it and the horizontal line y = fill_to.
void Ui::Polyline(const std::vector<D2D1_POINT_2F>& points, UINT32 color, float width, float opacity, float fill_to) {
  if (points.size() < 2) return;
  ComPtr<ID2D1PathGeometry> path;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(factory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
  const bool fill = fill_to >= 0.0f;
  sink->BeginFigure(fill ? D2D1::Point2F(points.front().x, fill_to) : points.front(),
                    fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLines(points.data() + (fill ? 0 : 1), static_cast<UINT32>(points.size() - (fill ? 0 : 1)));
  if (fill) sink->AddLine(D2D1::Point2F(points.back().x, fill_to));
  sink->EndFigure(fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
  if (FAILED(sink->Close())) return;
  brush_->SetColor(D2D1::ColorF(color));
  brush_->SetOpacity(opacity);
  if (fill) {
    target_->FillGeometry(path.Get(), brush_.Get());
  } else {
    target_->DrawGeometry(path.Get(), brush_.Get(), width);
  }
  brush_->SetOpacity(1.0f);
}

namespace {

// "-18.3" with a real minus sign, or an em dash when there's no value.
std::wstring Number(double value, const wchar_t* format = L"%.1f") {
  if (!std::isfinite(value)) return L"\x2014";
  wchar_t text[32];
  swprintf_s(text, format, value);
  std::wstring s = text;
  if (!s.empty() && s[0] == L'-') s[0] = L'\x2212';
  if (s == L"\x2212" L"0.0") s = L"0.0";
  return s;
}

}  // namespace

void Ui::DrawMain(const UiModel& m, Hit hover, D2D1_RECT_F panel) {
  Card(panel);
  const float x = panel.left + 20.0f, right = panel.right - 20.0f;
  static const wchar_t* const kTabs[] = {L"Levels", L"Loudness", L"Spectrum", L"Phase"};
  Segmented(x, panel.top + 14.0f, 34.0f, kTabs, 4, static_cast<int>(m.view), Hit::ViewLevels, hover, body_.Get());

  if (m.view == View::Levels || m.view == View::Loudness) {
    std::wstring label = m.view == View::Levels ? L"Reset clip lights" : L"Reset";
    Hit hit = m.view == View::Levels ? Hit::ResetClips : Hit::ResetLoudness;
    float bw = TextWidth(label, body_.Get()) + 28.0f;
    D2D1_RECT_F button = Rect(right - bw, panel.top + 14.0f, right, panel.top + 48.0f);
    Button(button, label, hover == hit);
    SetHit(hit, button);
  } else {
    static const wchar_t* const kSources[] = {L"Output", L"In 1/2"};
    Segmented(right, panel.top + 14.0f, 34.0f, kSources, 2, m.source, Hit::SourceOutput, hover, body_.Get(), true);
  }

  const float tiles_top = panel.bottom - 20.0f - 78.0f;
  D2D1_RECT_F area = Rect(x, panel.top + 62.0f, right, tiles_top - 16.0f);
  switch (m.view) {
    case View::Levels:
      DrawMeters(m, area);
      break;
    case View::Loudness:
      DrawLoudness(m, area);
      break;
    case View::Spectrum:
      DrawSpectrum(m, hover, area);
      break;
    case View::Phase:
      DrawPhase(m, area);
      break;
  }

  float tile_w = (panel.right - panel.left - 40.0f - 3.0f * 12.0f) / 4.0f;
  for (int i = 0; i < 4; ++i) {
    float tx = x + i * (tile_w + 12.0f);
    D2D1_RECT_F tile = Rect(tx, tiles_top, tx + tile_w, tiles_top + 78.0f);
    Fill(tile, kTile, 10.0f);
    Stroke(tile, kBorder, 10.0f);
    Text(kTileLabels[i], small_trimmed_.Get(), Rect(tx + 14.0f, tiles_top + 11.0f, tile.right - 8.0f, tiles_top + 28.0f),
         kSecondary);
    Text(m.tile_value[i], mono_large_.Get(), Rect(tx + 14.0f, tiles_top + 30.0f, tile.right - 8.0f, tiles_top + 54.0f),
         m.tile_attention[i] ? kAmber : kText);
    Text(m.tile_sub[i], small_trimmed_.Get(), Rect(tx + 14.0f, tiles_top + 54.0f, tile.right - 8.0f, tiles_top + 72.0f),
         kTertiary);
  }
}

void Ui::DrawMeters(const UiModel& m, D2D1_RECT_F area) {
  if (!m.meters_note.empty()) {
    Text(m.meters_note, small_trimmed_.Get(), Rect(area.left, area.top - 4.0f, area.right, area.top + 12.0f), kTertiary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
  const float group_top = area.top + 12.0f;
  const float meter_top = group_top + 26.0f;
  const float bar_height = std::max(80.0f, area.bottom - meter_top - 71.0f);

  // Center the scale and both meter groups.
  const float block_width = 30.0f + 14.0f + 2.0f * (2.0f * kMeterWidth + 12.0f) + 44.0f;
  const float block_left = area.left + std::max(0.0f, (area.right - area.left - block_width) / 2.0f);

  // dB scale, aligned with the bars
  float scale_right = block_left + 30.0f;
  for (int db : {0, -6, -12, -24, -48}) {
    float y = meter_top + 33.0f + bar_height * (1.0f - (db + kMeterRangeDb) / kMeterRangeDb);
    wchar_t label[8];
    swprintf_s(label, db ? L"\x2212%d" : L"%d", -db);
    Text(label, mono_.Get(), Rect(block_left - 10.0f, y - 8.0f, scale_right, y + 10.0f), kTertiary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
  }

  float gx = scale_right + 14.0f;
  Text(L"INPUTS", caps_.Get(), Rect(gx + 8.0f, group_top, gx + 200.0f, group_top + 16.0f), kSecondary);
  Meter(gx, meter_top, bar_height, m.meters[0], L"In 1", m.inputs_live);
  Meter(gx + kMeterWidth + 12.0f, meter_top, bar_height, m.meters[1], L"In 2", m.inputs_live);
  float divider = gx + 2.0f * kMeterWidth + 12.0f + 22.0f;
  Line(divider, group_top, divider, meter_top + 33.0f + bar_height + 26.0f, kBorder);
  float ox = divider + 22.0f;
  Text(L"OUTPUTS", caps_.Get(), Rect(ox + 8.0f, group_top, ox + 200.0f, group_top + 16.0f), kSecondary);
  Meter(ox, meter_top, bar_height, m.meters[2], L"Out 1", m.outputs_live);
  Meter(ox + kMeterWidth + 12.0f, meter_top, bar_height, m.meters[3], L"Out 2", m.outputs_live);
}

void Ui::DrawLoudness(const UiModel& m, D2D1_RECT_F area) {
  const LoudnessView& l = m.loudness;
  wchar_t duration[32];
  int seconds = static_cast<int>(l.seconds);
  swprintf_s(duration, L"over %d:%02d", seconds / 60, seconds % 60);
  struct Readout {
    const wchar_t* label;
    std::wstring value;
    const wchar_t* unit;
    std::wstring sub;
    bool attention;
  } readouts[5] = {
      {L"Momentary", Number(l.momentary), L"LUFS", L"max " + Number(l.momentary_max), false},
      {L"Short-term", Number(l.short_term), L"LUFS", L"max " + Number(l.short_term_max), false},
      {L"Integrated", Number(l.integrated), L"LUFS", duration, false},
      {L"Range", Number(std::isfinite(l.integrated) ? l.range : -INFINITY), L"LU", L"LRA", false},
      {L"True peak", Number(l.true_peak_max), L"dBTP", L"max", std::isfinite(l.true_peak_max) && l.true_peak_max > -1.0},
  };
  const float gap = 10.0f, tile_h = 80.0f;
  const float tile_w = (area.right - area.left - 4.0f * gap) / 5.0f;
  for (int i = 0; i < 5; ++i) {
    const Readout& r = readouts[i];
    float tx = area.left + i * (tile_w + gap);
    D2D1_RECT_F tile = Rect(tx, area.top, tx + tile_w, area.top + tile_h);
    Fill(tile, kTile, 10.0f);
    Stroke(tile, kBorder, 10.0f);
    Text(r.label, small_trimmed_.Get(), Rect(tx + 12.0f, area.top + 10.0f, tile.right - 6.0f, area.top + 27.0f), kSecondary);
    float vw = TextWidth(r.value, mono_large_.Get());
    Text(r.value, mono_large_.Get(), Rect(tx + 12.0f, area.top + 30.0f, tile.right - 4.0f, area.top + 54.0f),
         r.attention ? kAmber : kText);
    Text(r.unit, small_.Get(), Rect(tx + 16.0f + vw, area.top + 35.0f, tile.right - 4.0f, area.top + 52.0f), kTertiary);
    Text(r.sub, small_trimmed_.Get(), Rect(tx + 12.0f, area.top + 56.0f, tile.right - 6.0f, area.top + 73.0f), kTertiary);
  }

  // Loudness over the last minute: momentary (thin) and short-term (accent), with two common targets.
  D2D1_RECT_F g = Rect(area.left + 34.0f, area.top + tile_h + 20.0f, area.right - 96.0f, area.bottom - 50.0f);
  if (g.bottom - g.top < 60.0f) g.bottom = g.top + 60.0f;
  Fill(g, kTrack, 6.0f);
  constexpr float kTop = 0.0f, kBottom = -70.0f;  // the absolute gate
  auto y_of = [&](float lufs) {
    return g.top + (kTop - std::clamp(lufs, kBottom, kTop)) / (kTop - kBottom) * (g.bottom - g.top);
  };
  for (int db = -10; db >= -60; db -= 10) {
    float y = y_of(static_cast<float>(db));
    Line(g.left, y, g.right, y, kBorder);
    wchar_t label[8];
    swprintf_s(label, L"\x2212%d", -db);
    Text(label, mono_.Get(), Rect(area.left - 4.0f, y - 8.0f, g.left - 6.0f, y + 10.0f), kTertiary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
  struct Target {
    float lufs;
    const wchar_t* label;
  } targets[] = {{-14.0f, L"\x2212" L"14 streaming"}, {-23.0f, L"\x2212" L"23 EBU R128"}};
  for (const Target& t : targets) {
    float y = y_of(t.lufs);
    brush_->SetOpacity(0.6f);
    Line(g.left, y, g.right, y, kAmber);
    brush_->SetOpacity(1.0f);
    Text(t.label, small_.Get(), Rect(g.right + 8.0f, y - 8.0f, area.right + 4.0f, y + 10.0f), kSecondary);
  }
  auto curve = [&](const std::vector<float>& history) {
    std::vector<D2D1_POINT_2F> points;
    const size_t n = history.size();
    const float step = (g.right - g.left) / (LoudnessMeter::kHistory - 1);
    for (size_t i = 0; i < n; ++i) {
      if (history[i] <= -199.0f) continue;  // no value yet
      points.push_back(D2D1::Point2F(g.right - (n - 1 - i) * step, y_of(history[i])));
    }
    return points;
  };
  target_->PushAxisAlignedClip(g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  Polyline(curve(l.momentary_history), kSecondary, 1.0f, 0.7f);
  Polyline(curve(l.short_term_history), kAccent, 2.0f);
  target_->PopAxisAlignedClip();
  Stroke(g, kBorder, 6.0f);
  Text(L"\x2212" L"60 s", small_.Get(), Rect(g.left, g.bottom + 4.0f, g.left + 80.0f, g.bottom + 20.0f), kTertiary);
  Text(L"short-term", small_.Get(), Rect(g.left, g.bottom + 4.0f, g.right, g.bottom + 20.0f), kAccent,
       DWRITE_TEXT_ALIGNMENT_CENTER);
  Text(L"now", small_.Get(), Rect(g.right - 60.0f, g.bottom + 4.0f, g.right, g.bottom + 20.0f), kTertiary,
       DWRITE_TEXT_ALIGNMENT_TRAILING);
  Text(m.analysis_note, small_.Get(), Rect(area.left, area.bottom - 22.0f, area.right, area.bottom + 14.0f), kTertiary);
}

void Ui::DrawSpectrum(const UiModel& m, Hit hover, D2D1_RECT_F area) {
  static const wchar_t* const kSmoothing[] = {L"1/6 octave", L"1/3 octave"};
  Segmented(area.left, area.top - 2.0f, 28.0f, kSmoothing, 2, m.smoothing, Hit::Smoothing6, hover, small_.Get());
  Text(L"band level \x00B7 white: peak hold", small_.Get(), Rect(area.left, area.top + 4.0f, area.right, area.top + 22.0f),
       kTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);

  D2D1_RECT_F p = Rect(area.left + 34.0f, area.top + 40.0f, area.right - 6.0f, area.bottom - 50.0f);
  if (p.bottom - p.top < 60.0f) p.bottom = p.top + 60.0f;
  Fill(p, kTrack, 6.0f);
  constexpr float kRange = 90.0f;
  auto y_of = [&](float db) { return p.top + std::clamp(-db, 0.0f, kRange) / kRange * (p.bottom - p.top); };
  auto x_of = [&](double hz) { return p.left + static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)) * (p.right - p.left); };
  for (int db = -10; db >= -80; db -= 10) {
    float y = y_of(static_cast<float>(db));
    Line(p.left, y, p.right, y, kBorder);
    if (db % 20 == 0) {
      wchar_t label[8];
      swprintf_s(label, L"\x2212%d", -db);
      Text(label, mono_.Get(), Rect(area.left - 4.0f, y - 8.0f, p.left - 6.0f, y + 10.0f), kTertiary,
           DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
  }
  Text(L"0", mono_.Get(), Rect(area.left - 4.0f, p.top - 2.0f, p.left - 6.0f, p.top + 16.0f), kTertiary,
       DWRITE_TEXT_ALIGNMENT_TRAILING);
  struct Tick {
    double hz;
    const wchar_t* label;
  } ticks[] = {{20, L"20"},     {50, L"50"},    {100, L"100"}, {200, L"200"}, {500, L"500"},
               {1000, L"1k"},   {2000, L"2k"},  {5000, L"5k"}, {10000, L"10k"}, {20000, L"20k"}};
  for (const Tick& t : ticks) {
    float tx = x_of(t.hz);
    if (t.hz > 20 && t.hz < 20000) Line(tx, p.top, tx, p.bottom, kBorder);
    Text(t.label, mono_.Get(), Rect(tx - 24.0f, p.bottom + 4.0f, tx + 24.0f, p.bottom + 20.0f), kTertiary,
         DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  auto curve = [&](const std::vector<float>& db) {
    std::vector<D2D1_POINT_2F> points(db.size());
    const size_t n = db.size();
    for (size_t j = 0; j < n; ++j) {
      points[j] = D2D1::Point2F(p.left + (p.right - p.left) * j / (n - 1), y_of(db[j]));
    }
    return points;
  };
  if (m.spectrum.level.size() >= 2) {
    target_->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    std::vector<D2D1_POINT_2F> level = curve(m.spectrum.level);
    Polyline(level, kAccent, 1.0f, 0.18f, p.bottom);
    Polyline(level, kAccent, 1.5f);
    Polyline(curve(m.spectrum.peak), kText, 1.0f, 0.55f);
    target_->PopAxisAlignedClip();
  }
  Stroke(p, kBorder, 6.0f);
  Text(m.analysis_note, small_.Get(), Rect(area.left, area.bottom - 22.0f, area.right, area.bottom + 14.0f), kTertiary);
}

void Ui::DrawPhase(const UiModel& m, D2D1_RECT_F area) {
  const PhaseView& ph = m.phase;
  // Correlation: +1 mono, 0 unrelated, -1 out of phase.
  const float label_w = 92.0f, value_w = 56.0f;
  Text(L"Correlation", body_.Get(), Rect(area.left, area.top + 2.0f, area.left + label_w, area.top + 22.0f), kText);
  D2D1_RECT_F bar = Rect(area.left + label_w, area.top + 2.0f, area.right - value_w, area.top + 22.0f);
  Fill(bar, kTrack, 4.0f);
  const float cx = (bar.left + bar.right) / 2.0f, half = (bar.right - bar.left) / 2.0f - 2.0f;
  if (std::isfinite(ph.correlation)) {
    float vx = cx + static_cast<float>(ph.correlation) * half;
    Fill(Rect(std::min(cx, vx), bar.top + 3.0f, std::max(cx, vx) + 1.0f, bar.bottom - 3.0f),
         ph.correlation >= 0.0 ? kAccent : kRed, 2.0f);
    Fill(Rect(vx - 1.5f, bar.top + 1.0f, vx + 1.5f, bar.bottom - 1.0f), kText);
  }
  Line(cx, bar.top, cx, bar.bottom, kControl);
  Stroke(bar, kBorder, 4.0f);
  Text(Number(ph.correlation, L"%+.2f"), mono_.Get(), Rect(bar.right, area.top + 3.0f, area.right, area.top + 21.0f),
       std::isfinite(ph.correlation) && ph.correlation < 0.0 ? kRed : kText, DWRITE_TEXT_ALIGNMENT_TRAILING);
  struct Mark {
    float at;
    const wchar_t* label;
  } marks[] = {{-1.0f, L"\x2212" L"1"}, {-0.5f, L"\x2212" L"0.5"}, {0.0f, L"0"}, {0.5f, L"+0.5"}, {1.0f, L"+1"}};
  for (const Mark& mk : marks) {
    float mx = cx + mk.at * half;
    Text(mk.label, small_.Get(), Rect(mx - 24.0f, bar.bottom + 3.0f, mx + 24.0f, bar.bottom + 19.0f), kTertiary,
         DWRITE_TEXT_ALIGNMENT_CENTER);
  }

  // Goniometer: mid up, side across; left channel alone draws the upper-left diagonal, right the upper-right.
  const float top = area.top + 52.0f, bottom = area.bottom - 30.0f;
  const float size = std::max(80.0f, std::min(area.right - area.left, bottom - top));
  const float gx = (area.left + area.right) / 2.0f, gy = top + size / 2.0f;
  D2D1_RECT_F square = Rect(gx - size / 2.0f, top, gx + size / 2.0f, top + size);
  Fill(square, kTrack, 6.0f);
  const float r = size / 2.0f - 10.0f;
  Line(gx, gy - r, gx, gy + r, kBorder);
  Line(gx - r, gy, gx + r, gy, kBorder);
  Line(gx - r * 0.7071f, gy - r * 0.7071f, gx + r * 0.7071f, gy + r * 0.7071f, kBorder);
  Line(gx + r * 0.7071f, gy - r * 0.7071f, gx - r * 0.7071f, gy + r * 0.7071f, kBorder);
  Text(L"M", small_.Get(), Rect(gx + 4.0f, square.top + 4.0f, gx + 30.0f, square.top + 20.0f), kTertiary);
  Text(L"L", small_.Get(), Rect(gx - r * 0.7071f - 16.0f, gy - r * 0.7071f - 8.0f, gx - r * 0.7071f - 2.0f, gy - r * 0.7071f + 10.0f),
       kTertiary);
  Text(L"R", small_.Get(), Rect(gx + r * 0.7071f + 4.0f, gy - r * 0.7071f - 8.0f, gx + r * 0.7071f + 20.0f, gy - r * 0.7071f + 10.0f),
       kTertiary);
  Text(L"S", small_.Get(), Rect(square.right - 18.0f, gy + 2.0f, square.right - 4.0f, gy + 18.0f), kTertiary);

  const size_t n = ph.points.size() / 2;
  if (n) {
    target_->PushAxisAlignedClip(square, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    brush_->SetColor(D2D1::ColorF(kAccent));
    constexpr int kGroups = 8;
    for (int group = 0; group < kGroups; ++group) {
      brush_->SetOpacity(0.15f + 0.8f * (group + 1) / kGroups);  // newer points brighter
      size_t begin = n * group / kGroups, end = n * (group + 1) / kGroups;
      for (size_t i = begin; i < end; ++i) {
        float px = gx + ph.points[2 * i] * r, py = gy - ph.points[2 * i + 1] * r;
        target_->FillRectangle(Rect(px - 0.8f, py - 0.8f, px + 0.8f, py + 0.8f), brush_.Get());
      }
    }
    brush_->SetOpacity(1.0f);
    target_->PopAxisAlignedClip();
  }
  Stroke(square, kBorder, 6.0f);
  wchar_t gain[32];
  swprintf_s(gain, L"auto gain %+.0f dB", ph.gain_db);
  Text(gain, small_.Get(), Rect(square.left + 8.0f, square.bottom - 20.0f, square.right - 8.0f, square.bottom - 4.0f),
       kTertiary, DWRITE_TEXT_ALIGNMENT_TRAILING);
  Text(m.analysis_note, small_.Get(), Rect(area.left, area.bottom - 22.0f, area.right, area.bottom + 14.0f), kTertiary);
}

float Ui::DrawFirmware(const UiModel& m, Hit hover, float top, float cl, float cr) {
  const float il = cl + 20.0f, ir = cr - 20.0f;
  D2D1_RECT_F card = Rect(cl, top, cr, top + 204.0f);
  Card(card);
  Text(L"Firmware", heading_.Get(), Rect(il, top + 20.0f, ir - 130.0f, top + 42.0f), kText);
  if (!m.fw_installed.empty()) Badge(ir, top + 19.0f, m.fw_installed);

  Text(L"Image", body_.Get(), Rect(il, top + 58.0f, il + 60.0f, top + 78.0f), kSecondary);
  Text(m.fw_image.empty() ? L"None chosen" : m.fw_image, trimmed_.Get(), Rect(il + 64.0f, top + 58.0f, ir, top + 78.0f),
       m.fw_image.empty() ? kTertiary : kText, DWRITE_TEXT_ALIGNMENT_TRAILING);
  Text(m.fw_image_info, small_trimmed_.Get(), Rect(il, top + 81.0f, ir, top + 99.0f), m.fw_image_ok ? kTertiary : kAmber,
       DWRITE_TEXT_ALIGNMENT_TRAILING);

  float bw = (ir - il - 10.0f) / 2.0f;
  D2D1_RECT_F choose = Rect(il, top + 110.0f, il + bw, top + 144.0f);
  D2D1_RECT_F install = Rect(ir - bw, top + 110.0f, ir, top + 144.0f);
  Button(choose, L"Choose image\x2026", hover == Hit::ChooseImage, !m.fw_busy);
  Button(install, m.fw_busy ? L"Installing\x2026" : L"Install", hover == Hit::Install, m.fw_can_install, true);
  if (!m.fw_busy) SetHit(Hit::ChooseImage, choose);
  if (m.fw_can_install) SetHit(Hit::Install, install);

  float ny = top + 158.0f;
  if (m.fw_busy) {
    Fill(Rect(il, ny, ir, ny + 6.0f), kTrack, 3.0f);
    Fill(Rect(il, ny, il + (ir - il) * std::clamp(m.fw_progress, 0.0f, 1.0f), ny + 6.0f), kAccent, 3.0f);
    ny += 14.0f;
  }
  UINT32 color = m.fw_note_kind == 3 ? kRed : m.fw_note_kind == 2 ? kAmber : kTertiary;
  Text(m.fw_note, small_.Get(), Rect(il, ny, ir, card.bottom - 8.0f), color);
  return card.bottom;
}

void Ui::DrawLog(const UiModel& m, Hit hover, D2D1_RECT_F card) {
  Card(card);
  const float il = card.left + 20.0f, ir = card.right - 20.0f, top = card.top;
  Text(L"Dropout log", heading_.Get(), Rect(il, top + 20.0f, ir - 100.0f, top + 42.0f), kText);
  std::wstring open = L"Open log";
  float w = TextWidth(open, small_.Get()) + 24.0f;
  D2D1_RECT_F button = Rect(ir - w, top + 17.0f, ir, top + 45.0f);
  if (hover == Hit::OpenLog) Fill(button, kHover, 6.0f);
  Stroke(button, kControl, 6.0f);
  Text(open, small_.Get(), Rect(button.left, top + 22.0f, button.right, top + 40.0f), kText,
       DWRITE_TEXT_ALIGNMENT_CENTER);
  SetHit(Hit::OpenLog, button);

  const float footer_top = card.bottom - 34.0f;
  float y = top + 58.0f;
  if (m.log_times.empty()) {
    Text(m.log_empty, body_.Get(), Rect(il, y, ir, footer_top - 4.0f), kSecondary);
  } else {
    for (size_t i = 0; i < m.log_times.size() && y + 20.0f <= footer_top; ++i, y += 22.0f) {
      Text(m.log_times[i], mono_.Get(), Rect(il, y + 1.0f, il + 72.0f, y + 19.0f), kSecondary);
      Text(m.log_texts[i], small_trimmed_.Get(), Rect(il + 76.0f, y + 1.0f, ir, y + 19.0f), kText);
    }
  }
  Line(il, footer_top, ir, footer_top, kBorder);
  Text(m.log_footer, small_trimmed_.Get(), Rect(il, footer_top + 9.0f, ir, footer_top + 27.0f), kTertiary);
}

// The ASIO driver: whether it's installed, the buffer size it prefers (in samples over milliseconds at 48 kHz),
// its last open from the driver's log, and what blocks it.
float Ui::DrawDaw(const UiModel& m, Hit hover, float top, float cl, float cr) {
  const float il = cl + 20.0f, ir = cr - 20.0f;
  D2D1_RECT_F card = Rect(cl, top, cr, top + kDawHeight);
  Card(card);
  Text(L"DAW (ASIO)", heading_.Get(), Rect(il, top + 20.0f, ir - 130.0f, top + 42.0f), kText);
  if (!m.daw_badge.empty()) Badge(ir, top + 19.0f, m.daw_badge, m.daw_path_attention);
  Text(m.daw_path, small_trimmed_.Get(), Rect(il, top + 52.0f, ir, top + 70.0f), m.daw_path_attention ? kAmber : kTertiary);

  constexpr int kCount = static_cast<int>(std::size(kDriverBufferChoices));
  const float sw = (ir - il) / kCount;
  D2D1_RECT_F control = Rect(il, top + 76.0f, ir, top + 120.0f);
  Fill(control, kBg, 8.0f);
  Stroke(control, kControl, 8.0f);
  for (int i = 0; i < kCount; ++i) {
    D2D1_RECT_F seg = Rect(il + i * sw + 3.0f, control.top + 3.0f, il + (i + 1) * sw - 3.0f, control.bottom - 3.0f);
    Hit h = static_cast<Hit>(static_cast<int>(Hit::Buffer0) + i);
    const bool selected = i == m.daw_buffer;
    if (selected) {
      Fill(seg, kControl, 6.0f);
    } else if (hover == h) {
      Fill(seg, kHover, 6.0f);
    }
    Text(std::to_wstring(kDriverBufferChoices[i]), mono_.Get(), Rect(seg.left, seg.top + 3.0f, seg.right, seg.top + 21.0f),
         selected ? kText : kSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(std::to_wstring(kDriverBufferChoices[i] / 48) + L" ms", small_.Get(),
         Rect(seg.left, seg.top + 20.0f, seg.right, seg.bottom), selected ? kSecondary : kTertiary,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    SetHit(h, seg);
  }
  Text(m.daw_caption, small_.Get(), Rect(il, top + 128.0f, ir, top + 162.0f), kTertiary);

  Line(il, top + 168.0f, ir, top + 168.0f, kBorder);
  Text(m.daw_session, small_trimmed_.Get(), Rect(il, top + 174.0f, ir, top + 192.0f),
       m.daw_session_attention ? kAmber : kSecondary);
  Text(m.daw_session_detail, small_.Get(), Rect(il, top + 192.0f, ir, top + 242.0f), kTertiary);
  Text(m.daw_reminder, small_.Get(), Rect(il, top + 246.0f, ir, card.bottom - 6.0f),
       m.daw_reminder_attention ? kAmber : kTertiary);
  return card.bottom;
}

void Ui::DrawStartup(const UiModel& m, Hit hover, D2D1_RECT_F card) {
  Card(card);
  const float il = card.left + 20.0f, ir = card.right - 20.0f;
  const float cy = (card.top + card.bottom) / 2.0f;
  Text(L"Start with Windows", body_.Get(), Rect(il, cy - 20.0f, ir - 60.0f, cy), kText);
  Text(m.startup_note, small_trimmed_.Get(), Rect(il, cy + 1.0f, ir - 60.0f, cy + 19.0f), kTertiary);
  D2D1_RECT_F track = Rect(ir - 44.0f, cy - 12.0f, ir, cy + 12.0f);
  Toggle(track, m.startup, hover == Hit::Startup);
  SetHit(Hit::Startup, Rect(track.left - 8.0f, card.top + 4.0f, track.right + 4.0f, card.bottom - 4.0f));
}

Hit Ui::HitTest(POINT pixel) const {
  float x = pixel.x * 96.0f / dpi_, y = pixel.y * 96.0f / dpi_;
  for (int i = 1; i < static_cast<int>(Hit::Count); ++i) {
    if (Contains(hit_[i], x, y)) return static_cast<Hit>(i);
  }
  return Hit::None;
}
