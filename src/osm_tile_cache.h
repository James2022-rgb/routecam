#pragma once

// c++ system headers -----------------------------------
#include <memory>
#include <string>

// public project headers --------------------------------
#include "mnexus/public/types.h"

// project headers ---------------------------------------
#include "slippy_map.h"

namespace mnexus { class IDevice; }

namespace routecam {

/// Async OpenStreetMap raster tile cache. Three layers:
///   GPU texture (session)  <-  disk cache (persistent)  <-  tile.openstreetmap.org
///
/// `GetTile` never blocks: a miss enqueues a background fetch and
/// returns an invalid handle (the caller draws a placeholder until a
/// later frame). One worker thread performs disk reads, HTTPS
/// downloads (courteous to the OSM tile usage policy: identifying
/// User-Agent, single connection, permanent disk caching), and PNG
/// decodes; `Update` -- called once per frame on the owner thread --
/// turns completed decodes into mnexus textures.
///
/// Displaying these tiles requires "(c) OpenStreetMap contributors"
/// attribution; the map widget is responsible for showing it.
class OsmTileCache final {
public:
  /// `device` must outlive this instance. `cache_dir` is created on
  /// demand; pass e.g. `%LOCALAPPDATA%/RouteCam/tiles`.
  static std::unique_ptr<OsmTileCache> Create(
    mnexus::IDevice* device, std::string cache_dir);

  ~OsmTileCache();
  OsmTileCache(OsmTileCache const&) = delete;
  OsmTileCache& operator=(OsmTileCache const&) = delete;

  /// Returns the tile's texture if resident, otherwise an invalid
  /// handle (kicking off an async fetch on first request). Failed
  /// tiles stay invalid and are not retried this session.
  mnexus::TextureHandle GetTile(TileCoord coord);

  /// Drains completed fetches into GPU textures. Call once per frame
  /// from the thread that owns the mnexus device.
  void Update();

private:
  OsmTileCache();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routecam
