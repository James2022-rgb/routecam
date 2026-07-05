#pragma once

// public project headers --------------------------------
#include "mgpmf/public/gpmf_sample.h"

// project headers ---------------------------------------
#include "gps_timeline.h"
#include "osm_tile_cache.h"

namespace routecam {

/// Draws the corner minimap overlay for the current frame: OSM tiles
/// centred on `current_fix`, the full capture's track polyline, a
/// position marker, the mandatory "(c) OpenStreetMap" attribution,
/// and mouse-wheel zoom (updates `io_zoom`, clamped to sane slippy
/// zoom levels). Call between `ImGui::NewFrame` and `ImGui::Render`.
void DrawMinimap(OsmTileCache&      tiles,
                 GpsTimeline const& timeline,
                 mgpmf::Gps9 const& current_fix,
                 int&               io_zoom);

} // namespace routecam
