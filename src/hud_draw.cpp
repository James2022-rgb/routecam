// TU header --------------------------------------------
#include "hud_draw.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace routecam::hud {

namespace {

// ---- 7-segment digit primitive ---------------------------
//
// Drawn via ImDrawList::AddRectFilled rather than scaled-up ImGui
// font glyphs so the readout stays crisp at any size -- the atlas
// is baked at ~15 px and naive 2-4x text upscaling visibly blurs.

// Bit layout (LSB = segment a). Segments laid out like a calculator
// LCD:
//     aaaa
//    f    b
//    f    b
//     gggg
//    e    c
//    e    c
//     dddd
uint8_t SevenSegBits(int digit) {
  static const uint8_t k[10] = {
    0b0111111, // 0: abcdef
    0b0000110, // 1: bc
    0b1011011, // 2: abdeg
    0b1001111, // 3: abcdg
    0b1100110, // 4: bcfg
    0b1101101, // 5: acdfg
    0b1111101, // 6: acdefg
    0b0000111, // 7: abc
    0b1111111, // 8: abcdefg
    0b1101111, // 9: abcdfg
  };
  if (digit < 0 || digit > 9) return 0;
  return k[digit];
}

void DrawSevenSeg(ImDrawList* dl, ImVec2 p, float w, float h, int digit,
                  ImU32 col_on, ImU32 col_off) {
  uint8_t const segs = SevenSegBits(digit);
  float const t  = h * 0.12f;             // segment thickness
  float const vh = (h - 3.0f * t) * 0.5f; // vertical segment length
  float const hw = w - 2.0f * t;          // horizontal segment length

  auto seg = [&](ImVec2 a, ImVec2 b, bool on) {
    dl->AddRectFilled(a, b, on ? col_on : col_off, t * 0.35f);
  };
  // a: top horizontal
  seg(ImVec2(p.x + t,        p.y),
      ImVec2(p.x + t + hw,   p.y + t),                       (segs >> 0) & 1);
  // b: top-right vertical
  seg(ImVec2(p.x + w - t,    p.y + t),
      ImVec2(p.x + w,        p.y + t + vh),                  (segs >> 1) & 1);
  // c: bottom-right vertical
  seg(ImVec2(p.x + w - t,    p.y + t + vh + t),
      ImVec2(p.x + w,        p.y + t + vh + t + vh),         (segs >> 2) & 1);
  // d: bottom horizontal
  seg(ImVec2(p.x + t,        p.y + h - t),
      ImVec2(p.x + t + hw,   p.y + h),                       (segs >> 3) & 1);
  // e: bottom-left vertical
  seg(ImVec2(p.x,            p.y + t + vh + t),
      ImVec2(p.x + t,        p.y + t + vh + t + vh),         (segs >> 4) & 1);
  // f: top-left vertical
  seg(ImVec2(p.x,            p.y + t),
      ImVec2(p.x + t,        p.y + t + vh),                  (segs >> 5) & 1);
  // g: middle horizontal
  seg(ImVec2(p.x + t,        p.y + t + vh),
      ImVec2(p.x + t + hw,   p.y + t + vh + t),              (segs >> 6) & 1);
}

} // namespace

void DrawSpeedGauge(ImDrawList* dl, ImVec2 center, float radius, float kph) {
  ImU32 const kCWhite  = IM_COL32(255, 255, 255, 255);
  ImU32 const kCDim    = IM_COL32(180, 180, 180, 255);
  ImU32 const kCRimHi  = IM_COL32(170, 185, 200, 255);
  ImU32 const kCRimLo  = IM_COL32( 70,  85, 100, 255);
  ImU32 const kCFill   = IM_COL32( 80, 220, 120, 240);
  ImU32 const kCFillHi = IM_COL32(255, 110, 110, 240);
  ImU32 const kCFace   = IM_COL32( 15,  20,  30, 240);

  float const r_outer = radius;
  float const r_rim   = r_outer - radius * 0.027f;
  float const r_track = r_outer - radius * 0.107f;

  // 300 deg sweep, empty wedge at the bottom. ImGui PathArcTo uses
  // screen-space CW angles (0 = +X, pi/2 = down).
  constexpr float kSpeedoStart = 2.094395f;   // 2*pi/3  -- 7 o'clock
  constexpr float kSpeedoEnd   = 7.330382f;   // 7*pi/3  -- 5 o'clock
  constexpr float kSpeedoSweep = kSpeedoEnd - kSpeedoStart;  // 300 deg
  constexpr float kMaxKph      = 180.0f;
  constexpr float kRedlineKph  = 150.0f;

  // Dial face + rims.
  dl->AddCircleFilled(center, r_outer, kCFace, 64);
  dl->AddCircle      (center, r_outer, kCRimHi, 64, radius * 0.017f);
  dl->AddCircle      (center, r_outer - radius * 0.08f, kCRimLo, 64, 1.0f);

  // Empty track ring so the active fill stands out.
  float const track_w = radius * 0.053f;
  dl->PathArcTo(center, r_track, kSpeedoStart, kSpeedoEnd, 64);
  dl->PathStroke(IM_COL32(45, 55, 70, 220), false, track_w);

  // Filled speed arc -- swaps to redline color past the threshold.
  if (kph > 0.0f) {
    float const t = std::min(kph / kMaxKph, 1.0f);
    float const fill_end = kSpeedoStart + t * kSpeedoSweep;
    bool  const redlined = kph >= kRedlineKph;
    dl->PathArcTo(center, r_track, kSpeedoStart, fill_end, 64);
    dl->PathStroke(redlined ? kCFillHi : kCFill, false, track_w);
  }

  // Major + minor ticks: 0..180 every 10 km/h; labels every 20.
  ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
  float const label_h = radius * 0.14f;
  for (int i = 0; i <= 18; ++i) {
    float const tick_kph = i * 10.0f;
    float const angle    = kSpeedoStart + (tick_kph / kMaxKph) * kSpeedoSweep;
    float const cos_a    = std::cos(angle);
    float const sin_a    = std::sin(angle);
    bool  const major    = (i % 2) == 0;
    float const tick_len = (major ? 0.073f : 0.04f) * radius;
    ImVec2 const p_outer(center.x + r_rim * cos_a,
                         center.y + r_rim * sin_a);
    ImVec2 const p_inner(center.x + (r_rim - tick_len) * cos_a,
                         center.y + (r_rim - tick_len) * sin_a);
    dl->AddLine(p_inner, p_outer,
                major ? kCWhite : IM_COL32(140, 150, 160, 200),
                (major ? 0.012f : 0.007f) * radius);
    if (major) {
      char lbl[8];
      std::snprintf(lbl, sizeof(lbl), "%d", static_cast<int>(tick_kph));
      ImVec2 const sz = font->CalcTextSizeA(label_h, FLT_MAX, 0.0f, lbl);
      float const r_lbl = r_rim - tick_len - radius * 0.08f;
      ImVec2 const lp(center.x + r_lbl * cos_a - sz.x * 0.5f,
                      center.y + r_lbl * sin_a - sz.y * 0.5f);
      dl->AddText(font, label_h, lp, kCDim, lbl);
    }
  }

  // Crisp central 7-segment readout.
  int const kph_int = std::clamp(static_cast<int>(kph + 0.5f), 0, 999);
  char digits[4]{};
  std::snprintf(digits, sizeof(digits), "%3d", kph_int);

  float const digit_w   = radius * 0.30f;
  float const digit_h   = radius * 0.55f;
  float const digit_gap = digit_w * 0.20f;
  int const num_digits  = 3;
  float const total_w   = num_digits * digit_w + (num_digits - 1) * digit_gap;
  float const digits_y  = center.y - digit_h * 0.5f - radius * 0.05f;
  float const digits_x0 = center.x - total_w * 0.5f;

  ImU32 const seg_off = IM_COL32(255, 255, 255, 24);
  for (int i = 0; i < num_digits; ++i) {
    ImVec2 const dp(digits_x0 + i * (digit_w + digit_gap), digits_y);
    if (digits[i] == ' ') {
      // Leading-blank slots render as fully-off segments to keep the
      // classic 7-seg "ghost-8" aesthetic.
      DrawSevenSeg(dl, dp, digit_w, digit_h, 8, seg_off, seg_off);
    } else {
      DrawSevenSeg(dl, dp, digit_w, digit_h, digits[i] - '0', kCWhite, seg_off);
    }
  }

  // "km/h" caption below the digits.
  char const* unit = "km/h";
  float const unit_h  = radius * 0.16f;
  ImVec2 const unit_sz = font->CalcTextSizeA(unit_h, FLT_MAX, 0.0f, unit);
  dl->AddText(font, unit_h,
              ImVec2(center.x - unit_sz.x * 0.5f,
                     digits_y + digit_h + radius * 0.04f),
              kCDim, unit);

  // Hub.
  dl->AddCircleFilled(center, radius * 0.033f, kCRimHi, 16);
}

void DrawTrackPolyline(ImDrawList* dl, ImVec2 const* points, int count,
                       int traversed_count, float scale) {
  if (count < 2) return;
  // Dark casing under a bright line for contrast on any map style.
  dl->AddPolyline(points, count, IM_COL32(20, 40, 90, 200),
                  ImDrawFlags_None, 5.0f * scale);
  dl->AddPolyline(points, count, IM_COL32(60, 140, 255, 255),
                  ImDrawFlags_None, 2.5f * scale);
  if (traversed_count >= 2) {
    dl->AddPolyline(points, traversed_count, IM_COL32(255, 110, 110, 255),
                    ImDrawFlags_None, 2.5f * scale);
  }
}

void DrawPositionMarker(ImDrawList* dl, ImVec2 p, float scale) {
  dl->AddCircleFilled(p, 7.0f * scale, IM_COL32(255, 255, 255, 255));
  dl->AddCircleFilled(p, 5.0f * scale, IM_COL32(230, 60, 60, 255));
}

void DrawOsmAttribution(ImDrawList* dl, ImVec2 map_min, ImVec2 map_max,
                        float scale) {
  (void)map_min;
  char const* const attribution = "(c) OpenStreetMap";
  ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
  float const text_h = font->FontSize * scale;
  ImVec2 const text_size = font->CalcTextSizeA(text_h, FLT_MAX, 0.0f, attribution);
  ImVec2 const text_pos  = ImVec2(map_max.x - text_size.x - 4.0f * scale,
                                  map_max.y - text_size.y - 2.0f * scale);
  dl->AddRectFilled(ImVec2(text_pos.x - 3.0f * scale, text_pos.y - 1.0f * scale),
                    map_max, IM_COL32(255, 255, 255, 160));
  dl->AddText(font, text_h, text_pos, IM_COL32(60, 60, 60, 255), attribution);
}

void DrawMapFrame(ImDrawList* dl, ImVec2 map_min, ImVec2 map_max) {
  dl->AddRect(map_min, map_max, IM_COL32(255, 255, 255, 90));
}

} // namespace routecam::hud
