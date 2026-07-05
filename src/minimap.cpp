// TU header --------------------------------------------
#include "minimap.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <cmath>
#include <vector>

// external headers --------------------------------------
#include "imgui.h"

// project headers ---------------------------------------
#include "hud_draw.h"
#include "slippy_map.h"

namespace routecam {

namespace {

constexpr float kMapSizePx  = 280.0f;
constexpr float kPaddingPx  = 16.0f;
constexpr int   kMinZoom    = 3;
constexpr int   kMaxZoom    = 19;

// Cap the polyline's draw cost for multi-hour captures.
constexpr size_t kMaxPolylinePoints = 1500;

} // namespace

void DrawMinimap(OsmTileCache&      tiles,
                 GpsTimeline const& timeline,
                 mgpmf::Gps9 const& current_fix,
                 double             current_pts_seconds,
                 int&               io_zoom) {
  io_zoom = std::clamp(io_zoom, kMinZoom, kMaxZoom);

  ImGuiViewport const* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
    ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kPaddingPx,
           viewport->WorkPos.y + viewport->WorkSize.y - kPaddingPx),
    ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(kMapSizePx, kMapSizePx), ImGuiCond_Always);
  ImGuiWindowFlags const flags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  bool const open = ImGui::Begin("##Minimap", nullptr, flags);
  ImGui::PopStyleVar();
  if (!open) {
    ImGui::End();
    return;
  }

  // Wheel over the map adjusts zoom.
  if (ImGui::IsWindowHovered()) {
    float const wheel = ImGui::GetIO().MouseWheel;
    if (wheel > 0.0f)      io_zoom = std::min(io_zoom + 1, kMaxZoom);
    else if (wheel < 0.0f) io_zoom = std::max(io_zoom - 1, kMinZoom);
  }

  ImVec2 const map_min    = ImGui::GetWindowPos();
  ImVec2 const map_max    = ImVec2(map_min.x + kMapSizePx, map_min.y + kMapSizePx);
  ImVec2 const map_center = ImVec2(map_min.x + kMapSizePx * 0.5f,
                                   map_min.y + kMapSizePx * 0.5f);
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  draw->PushClipRect(map_min, map_max, true);

  // Fractional tile position of the map centre.
  double center_tx = 0.0, center_ty = 0.0;
  LatLonToTileXY(current_fix.latitude, current_fix.longitude, io_zoom,
                 center_tx, center_ty);

  // Projects a fractional tile coordinate to screen pixels. Deltas
  // from the centre stay small, so float precision is fine after
  // the double-precision subtraction.
  auto const TileToScreen = [&](double tx, double ty) -> ImVec2 {
    return ImVec2(
      map_center.x + static_cast<float>((tx - center_tx) * kOsmTileSizePx),
      map_center.y + static_cast<float>((ty - center_ty) * kOsmTileSizePx));
  };

  // ----- Tiles -----------------------------------------
  int const n_tiles     = 1 << io_zoom;
  int const tiles_half  = static_cast<int>(kMapSizePx / (2.0f * kOsmTileSizePx)) + 1;
  int const center_tile_x = static_cast<int>(std::floor(center_tx));
  int const center_tile_y = static_cast<int>(std::floor(center_ty));
  for (int dy = -tiles_half; dy <= tiles_half; ++dy) {
    for (int dx = -tiles_half; dx <= tiles_half; ++dx) {
      int const ty = center_tile_y + dy;
      if (ty < 0 || ty >= n_tiles) continue;             // outside Mercator
      int const tx_wrapped =
        ((center_tile_x + dx) % n_tiles + n_tiles) % n_tiles;  // wrap around the antimeridian

      ImVec2 const top_left = TileToScreen(
        static_cast<double>(center_tile_x + dx), static_cast<double>(ty));
      ImVec2 const bottom_right =
        ImVec2(top_left.x + kOsmTileSizePx, top_left.y + kOsmTileSizePx);
      if (bottom_right.x < map_min.x || top_left.x > map_max.x ||
          bottom_right.y < map_min.y || top_left.y > map_max.y) continue;

      mnexus::TextureHandle const texture =
        tiles.GetTile(TileCoord{ .z = io_zoom, .x = tx_wrapped, .y = ty });
      if (texture.IsValid()) {
        draw->AddImage(static_cast<ImTextureID>(texture.Get()),
                       top_left, bottom_right);
      } else {
        draw->AddRectFilled(top_left, bottom_right, IM_COL32(48, 48, 48, 255));
      }
    }
  }

  // ----- Track polyline (traversed part highlighted) ----
  {
    std::span<GpsPoint const> const points = timeline.points();
    size_t const stride = std::max<size_t>(1, points.size() / kMaxPolylinePoints);
    std::vector<ImVec2> screen_points;
    screen_points.reserve(points.size() / stride + 2);
    int traversed = 0;
    for (size_t i = 0; i < points.size(); i += stride) {
      double tx = 0.0, ty = 0.0;
      LatLonToTileXY(points[i].gps.latitude, points[i].gps.longitude, io_zoom, tx, ty);
      screen_points.push_back(TileToScreen(tx, ty));
      if (points[i].pts_seconds <= current_pts_seconds) {
        traversed = static_cast<int>(screen_points.size());
      }
    }
    hud::DrawTrackPolyline(draw, screen_points.data(),
                           static_cast<int>(screen_points.size()),
                           traversed, /*scale=*/1.0f);
  }

  // ----- Position marker -------------------------------
  hud::DrawPositionMarker(draw, map_center, /*scale=*/1.0f);

  // ----- Attribution (required by the OSM tile policy) --
  hud::DrawOsmAttribution(draw, map_min, map_max, /*scale=*/1.0f);

  draw->PopClipRect();
  hud::DrawMapFrame(draw, map_min, map_max);

  ImGui::End();
}

} // namespace routecam
