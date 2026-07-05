#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace routecam {

/// Offline OpenStreetMap minimap helper for the burn-in transcoder.
/// Given a list of GPS positions, fetches the covering OSM tiles
/// (`tile.openstreetmap.org`) into a local disk cache, stitches them
/// into a single CPU RGBA8 image, and exposes a projection from
/// (lat, lng) to pixel coordinates inside that image. The caller
/// owns GPU upload.
///
/// All operations are synchronous; `Prepare` blocks while the HTTP
/// fetches run. The disk layout (`<cache_dir>/<z>/<x>/<y>.png`)
/// matches `OsmTileCache`'s, so both share one cache directory.
class OsmMinimap final {
public:
  struct GpsPoint final {
    double lat = 0.0;
    double lng = 0.0;
  };

  struct PrepareConfig final {
    /// Chronological list of GPS positions. Bounding box is
    /// derived from these; an empty route is invalid.
    std::vector<GpsPoint> route;
    /// Disk path under which tiles get cached as
    /// `<cache_dir>/<z>/<x>/<y>.png`. Directories are created on
    /// demand. Persist across runs so repeated transcodes of the
    /// same area do not re-download.
    std::string cache_dir;
    /// HTTP User-Agent header. OpenStreetMap policy requires a
    /// descriptive identifier so site operators can contact
    /// abusive clients.
    std::string user_agent = "RouteCam/0.1 (personal action-cam viewer)";
    /// Lowest / highest zoom levels the route is allowed to use.
    /// Higher zoom = more detail but more tiles.
    int min_zoom = 12;
    int max_zoom = 17;
    /// Soft cap on tiles fetched in each axis. When the natural
    /// bounding box would exceed this at the chosen zoom, Prepare
    /// zooms out until it fits.
    int max_tiles_x = 4;
    int max_tiles_y = 4;
  };

  /// Synchronously fetches missing tiles, decodes PNGs, stitches
  /// them into one RGBA8 image. Returns nullptr on configuration,
  /// network, or decode error.
  static std::unique_ptr<OsmMinimap> Prepare(PrepareConfig const& config);

  ~OsmMinimap();
  OsmMinimap(OsmMinimap const&) = delete;
  OsmMinimap& operator=(OsmMinimap const&) = delete;

  /// Stitched RGBA8 base map dimensions. Always a multiple of 256.
  uint32_t base_map_width() const;
  uint32_t base_map_height() const;
  /// Top-to-bottom raster of width * height RGBA8 samples.
  uint8_t const* base_map_rgba() const;

  /// Project a GPS position into pixel coordinates inside the
  /// stitched base map. `out_*` are always set to the unclamped
  /// projected position; the return value is false when the point
  /// lies outside the base map's bounding tiles.
  bool ProjectGpsToPixel(double lat, double lng,
                         float& out_x, float& out_y) const;

private:
  OsmMinimap() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routecam
