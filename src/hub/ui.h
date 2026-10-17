// Direct2D rendering of the hub window (layout follows the design mockup:
// https://claude.ai/artifact/8DcezW8oupDw8DLSz3iwCb). Everything is drawn in DIPs.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

struct MeterView {
  float level_db = -120.0f;  // falls back at a fixed rate after peaks
  float peak_db = -120.0f;   // peak-hold line
  float hold_seconds = 0.0f;
  bool clip = false;
};

enum class View { Levels, Loudness, Spectrum, Phase };

// Analysis values, -infinity (or NaN for the correlation) where there's nothing to show.
struct LoudnessView {
  double momentary = 0, short_term = 0, integrated = 0, range = 0;
  double momentary_max = 0, short_term_max = 0, true_peak_max = 0;
  double seconds = 0;                     // measured since the last reset
  std::vector<float> momentary_history;  // LUFS every 100 ms, oldest first, up to a minute
  std::vector<float> short_term_history;
};

struct SpectrumView {
  std::vector<float> level, peak;  // dB per column, log-spaced from 20 Hz to 20 kHz
};

struct PhaseView {
  double correlation = 0;
  double gain_db = 0;          // the goniometer's automatic gain
  std::vector<float> points;   // x, y pairs, oldest first, about -1..1
};

struct UiModel {
  std::wstring subtitle;
  std::wstring status;   // first pill
  int status_kind = 0;   // 0 neutral, 1 good (accent dot), 2 attention (amber dot)
  std::wstring rate;     // second pill: the Mbox's sample rate; empty hides it
  int rate_kind = 0;     // 0 neutral, 2 attention (Windows' playback and recording rates differ)
  bool side_open = false;       // the settings cog's side column: firmware, dropout log, DAW, startup
  bool side_attention = false;  // something there needs a look (amber dot on the cog while it's closed)
  MeterView meters[4];   // In 1, In 2, Out 1, Out 2
  bool inputs_live = false;
  bool outputs_live = false;
  std::wstring meters_note;  // why some meters are off
  std::wstring tile_value[4];
  std::wstring tile_sub[4];
  bool tile_attention[4] = {};

  // Firmware card
  std::wstring fw_installed;   // badge, e.g. "v0.22 installed"; empty hides it
  std::wstring fw_image;       // chosen image's file name
  std::wstring fw_image_info;  // its version and size, or what's wrong with it
  bool fw_image_ok = false;
  bool fw_can_install = false;
  bool fw_busy = false;
  float fw_progress = 0.0f;    // 0..1 while busy
  std::wstring fw_note;        // progress text, or advice
  int fw_note_kind = 0;        // 0 neutral, 2 attention (amber), 3 error (red)

  // Dropout log card
  std::vector<std::wstring> log_times;  // newest first
  std::vector<std::wstring> log_texts;
  std::wstring log_empty;
  std::wstring log_footer;

  // Analysis views (the main panel's other tabs)
  View view = View::Levels;
  int source = 0;     // spectrum and phase: 0 the output, 1 inputs 1/2
  int smoothing = 0;  // spectrum: 0 1/6 octave, 1 1/3 octave
  std::wstring analysis_note;
  LoudnessView loudness;
  SpectrumView spectrum;
  PhaseView phase;

  // DAW (ASIO) card
  std::wstring daw_badge;            // "Installed", "Not installed", ...
  std::wstring daw_path;             // the driver's DLL, or how to install it
  bool daw_path_attention = false;
  int daw_buffer = -1;               // the driver's preferred size, as an index into kDriverBufferChoices; -1 none
  std::wstring daw_caption;
  std::wstring daw_session;          // the last open, from the driver's log: one line
  std::wstring daw_session_detail;   // up to two more
  bool daw_session_attention = false;
  std::wstring daw_reminder;         // that the input meters block the driver
  bool daw_reminder_attention = false;

  bool startup = false;
  std::wstring startup_note;
};

enum class Hit {
  None,
  ResetClips,
  ChooseImage,
  Install,
  OpenLog,
  Startup,
  Buffer0,  // the driver's buffer sizes, in kDriverBufferChoices order
  Buffer1,
  Buffer2,
  Buffer3,
  Buffer4,
  ViewLevels,  // the four tabs, in View order
  ViewLoudness,
  ViewSpectrum,
  ViewPhase,
  ResetLoudness,
  SourceOutput,
  SourceInputs,
  Smoothing6,
  Smoothing3,
  Settings,  // the cog: shows or hides the side column
  Count
};

class Ui {
 public:
  HRESULT Init(HWND hwnd);
  void Resize();
  void SetDpi(UINT dpi);
  HRESULT Paint(const UiModel& model, Hit hover);
  // Draws the same view into a PNG (for --snapshot; works without a visible desktop).
  HRESULT RenderToPng(const UiModel& model, UINT width_px, UINT height_px, const wchar_t* path);
  Hit HitTest(POINT pixel) const;

  // Smallest client size (in DIPs) at which the layout fits, with the side column open or closed.
  static constexpr float MinWidth(bool side_open) { return side_open ? 940.0f : 640.0f; }
  static constexpr float MinHeight(bool side_open) { return side_open ? 850.0f : 560.0f; }
  // What the side column adds to the width: the column and the gap before it.
  static constexpr float kSideWidth = 356.0f;

 private:
  void Draw(const UiModel& model, Hit hover);
  void DrawMain(const UiModel& m, Hit hover, D2D1_RECT_F panel);
  void DrawMeters(const UiModel& m, D2D1_RECT_F area);
  void DrawLoudness(const UiModel& m, D2D1_RECT_F area);
  void DrawSpectrum(const UiModel& m, Hit hover, D2D1_RECT_F area);
  void DrawPhase(const UiModel& m, D2D1_RECT_F area);
  // A row of options in a track; the selected one filled. Returns the control's width.
  float Segmented(float x, float y, float height, const wchar_t* const* labels, int count, int selected, Hit first,
                  Hit hover, IDWriteTextFormat* format, bool right_aligned = false);
  void Polyline(const std::vector<D2D1_POINT_2F>& points, UINT32 color, float width, float opacity = 1.0f,
                float fill_to = -1.0f);
  float DrawFirmware(const UiModel& m, Hit hover, float top, float cl, float cr);
  void DrawLog(const UiModel& m, Hit hover, D2D1_RECT_F card);
  float DrawDaw(const UiModel& m, Hit hover, float top, float cl, float cr);
  void DrawStartup(const UiModel& m, Hit hover, D2D1_RECT_F card);
  HRESULT CreateTarget();
  HRESULT CreateFormats();
  Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight);

  void Text(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect, UINT32 color,
            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
  float TextWidth(const std::wstring& text, IDWriteTextFormat* format);
  void Fill(D2D1_RECT_F rect, UINT32 color, float radius = 0.0f);
  void Stroke(D2D1_RECT_F rect, UINT32 color, float radius = 0.0f, float width = 1.0f);
  void Line(float x0, float y0, float x1, float y1, UINT32 color, float width = 1.0f);
  void Circle(float cx, float cy, float r, UINT32 color);

  float Pill(float right, float y, const std::wstring& text, UINT32 dot, bool mono);  // returns its left edge
  void Badge(float right, float top, const std::wstring& text, bool attention = false);
  void Toggle(D2D1_RECT_F rect, bool on, bool hover);
  void Gear(float cx, float cy, UINT32 color, UINT32 hole);
  void Button(D2D1_RECT_F rect, const std::wstring& text, bool hover, bool enabled = true, bool primary = false);
  void Meter(float x, float top, float bar_height, const MeterView& meter, const wchar_t* label, bool live);
  void Card(D2D1_RECT_F rect);
  void SetHit(Hit hit, D2D1_RECT_F rect) { hit_[static_cast<int>(hit)] = rect; }

  HWND hwnd_ = nullptr;
  float dpi_ = 96.0f;
  Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> hwnd_target_;
  Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;  // what Draw() draws on: the window or a PNG bitmap
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> title_, heading_, body_, small_, caps_, mono_, mono_large_, trimmed_,
      small_trimmed_;

  // Hit areas from the last paint, in DIPs.
  D2D1_RECT_F hit_[static_cast<int>(Hit::Count)] = {};
};
