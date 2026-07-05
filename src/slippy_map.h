#pragma once

// c++ system headers -----------------------------------
#include <cmath>
#include <cstdint>

namespace routecam {

/// OSM slippy-map tile size in pixels (fixed by the tile servers).
constexpr int kOsmTileSizePx = 256;

/// Identifies one slippy-map tile. `x` / `y` are in [0, 2^z).
struct TileCoord final {
  int z = 0;
  int x = 0;
  int y = 0;

  bool operator==(TileCoord const&) const = default;
};

struct TileCoordHash final {
  size_t operator()(TileCoord const& t) const {
    // z < 32, x / y < 2^z <= 2^22 in practice; pack into one word.
    return (static_cast<size_t>(t.z) << 58) ^
           (static_cast<size_t>(static_cast<uint32_t>(t.x)) << 29) ^
            static_cast<size_t>(static_cast<uint32_t>(t.y));
  }
};

/// Fractional tile coordinates of a WGS84 position at zoom `z`
/// (standard Web Mercator slippy-map convention). The integer parts
/// are the containing tile's x / y; the fractions locate the point
/// inside that tile.
inline void LatLonToTileXY(double lat_deg, double lon_deg, int z,
                           double& out_tx, double& out_ty) {
  double const n       = static_cast<double>(1 << z);
  double const lat_rad = lat_deg * 3.14159265358979323846 / 180.0;
  out_tx = (lon_deg + 180.0) / 360.0 * n;
  out_ty = (1.0 - std::asinh(std::tan(lat_rad)) / 3.14159265358979323846) / 2.0 * n;
}

} // namespace routecam
