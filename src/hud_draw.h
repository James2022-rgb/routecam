#pragma once

// external headers --------------------------------------
#include "imgui.h"

namespace routecam::hud {

/// Shared HUD drawing primitives used by BOTH the playback overlays
/// and the burn-in transcode overlay, so the two render identically.
/// All sizes scale off the caller-provided geometry (gauge radius /
/// map rect), making the same code crisp at 720p windows and 4K
/// encodes alike.

/// Cockpit-style speed gauge: dial face, 300-degree arc with fill +
/// redline, ticks + labels, crisp 7-segment central readout, and a
/// "km/h" caption. Text is drawn at sizes proportional to `radius`.
void DrawSpeedGauge(ImDrawList* dl, ImVec2 center, float radius, float kph);

/// Track polyline in the shared style: dark casing under a bright
/// blue line. `traversed_count` > 0 additionally highlights the
/// first N points (the already-driven part) in red; pass 0 to skip.
void DrawTrackPolyline(ImDrawList* dl, ImVec2 const* points, int count,
                       int traversed_count, float scale);

/// Current-position marker: white disc with a red core.
void DrawPositionMarker(ImDrawList* dl, ImVec2 p, float scale);

/// Mandatory "(c) OpenStreetMap" attribution chip, bottom-right
/// inside the map rect.
void DrawOsmAttribution(ImDrawList* dl, ImVec2 map_min, ImVec2 map_max,
                        float scale);

/// Thin light frame around the map rect.
void DrawMapFrame(ImDrawList* dl, ImVec2 map_min, ImVec2 map_max);

} // namespace routecam::hud
