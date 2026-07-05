// TU header --------------------------------------------
#include "osm_minimap.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// external headers -------------------------------------
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <winhttp.h>
#endif

// public project headers -------------------------------
#include "mbase/public/log.h"
#include "mimage/public/mimage.h"

namespace routecam {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct TileXY final {
  int x = 0;
  int y = 0;
};

TileXY LatLngToTile(double lat, double lng, int zoom) {
  double const lat_rad = lat * kPi / 180.0;
  double const n = std::pow(2.0, zoom);
  TileXY t;
  t.x = static_cast<int>(std::floor((lng + 180.0) / 360.0 * n));
  t.y = static_cast<int>(std::floor(
    (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) * 0.5 * n));
  return t;
}

void LatLngToTileF(double lat, double lng, int zoom,
                   double& out_x, double& out_y) {
  double const lat_rad = lat * kPi / 180.0;
  double const n = std::pow(2.0, zoom);
  out_x = (lng + 180.0) / 360.0 * n;
  out_y = (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) * 0.5 * n;
}

#if defined(_WIN32)

// HTTPS GET. Returns true on HTTP 200 with the body in `out_bytes`.
// Non-200 status is reported via `out_status` and treated as failure
// but the body (often a short text error) is still left in
// `out_bytes` so callers can log it.
bool WinHttpGet(wchar_t const* host,
                wchar_t const* path,
                wchar_t const* user_agent,
                std::vector<uint8_t>& out_bytes,
                int& out_status) {
  out_bytes.clear();
  out_status = 0;

  HINTERNET session = WinHttpOpen(user_agent,
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;

  HINTERNET conn = WinHttpConnect(session, host,
                                  INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!conn) {
    WinHttpCloseHandle(session);
    return false;
  }

  HINTERNET req = WinHttpOpenRequest(conn, L"GET", path,
                                     NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
  if (!req) {
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return false;
  }

  BOOL ok = WinHttpSendRequest(req,
                               WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0,
                               0, 0);
  if (ok) ok = WinHttpReceiveResponse(req, NULL);
  if (!ok) {
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD status = 0;
  DWORD status_sz = sizeof(status);
  WinHttpQueryHeaders(req,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                      WINHTTP_NO_HEADER_INDEX);
  out_status = static_cast<int>(status);

  uint8_t buf[8192];
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(req, buf, sizeof(buf), &read)) break;
    if (read == 0) break;
    out_bytes.insert(out_bytes.end(), buf, buf + read);
  }

  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return out_status == 200;
}

std::wstring Utf8ToWide(std::string const& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                              static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(),
                      static_cast<int>(s.size()), w.data(), n);
  return w;
}

#endif // _WIN32

bool ReadFileBytes(std::filesystem::path const& p,
                   std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  auto const size = static_cast<size_t>(f.tellg());
  f.seekg(0, std::ios::beg);
  out.resize(size);
  f.read(reinterpret_cast<char*>(out.data()), size);
  return f.gcount() == static_cast<std::streamsize>(size);
}

bool WriteFileBytes(std::filesystem::path const& p,
                    uint8_t const* data, size_t size) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<char const*>(data), size);
  return f.good();
}

} // namespace

struct OsmMinimap::Impl final {
  int      zoom   = 0;
  int      tx_min = 0;
  int      tx_max = 0;
  int      ty_min = 0;
  int      ty_max = 0;
  uint32_t width  = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

std::unique_ptr<OsmMinimap> OsmMinimap::Prepare(PrepareConfig const& config) {
  if (config.route.empty()) {
    MBASE_LOG_ERROR("OsmMinimap::Prepare: route is empty");
    return nullptr;
  }
  if (config.cache_dir.empty()) {
    MBASE_LOG_ERROR("OsmMinimap::Prepare: cache_dir is empty");
    return nullptr;
  }

  double lat_min =  90.0;
  double lat_max = -90.0;
  double lng_min =  180.0;
  double lng_max = -180.0;
  for (auto const& p : config.route) {
    lat_min = std::min(lat_min, p.lat);
    lat_max = std::max(lat_max, p.lat);
    lng_min = std::min(lng_min, p.lng);
    lng_max = std::max(lng_max, p.lng);
  }

  // OSM y axis is inverted (north = smaller y), so the bounding
  // box's NW corner comes from (lat_max, lng_min).
  int zoom = config.max_zoom;
  int tx_min = 0;
  int tx_max = 0;
  int ty_min = 0;
  int ty_max = 0;
  while (zoom >= config.min_zoom) {
    auto const nw = LatLngToTile(lat_max, lng_min, zoom);
    auto const se = LatLngToTile(lat_min, lng_max, zoom);
    tx_min = nw.x;
    tx_max = se.x;
    ty_min = nw.y;
    ty_max = se.y;
    int const tw = tx_max - tx_min + 1;
    int const th = ty_max - ty_min + 1;
    if (tw <= config.max_tiles_x && th <= config.max_tiles_y) break;
    --zoom;
  }
  if (zoom < config.min_zoom) {
    zoom = config.min_zoom;
    auto const nw = LatLngToTile(lat_max, lng_min, zoom);
    auto const se = LatLngToTile(lat_min, lng_max, zoom);
    tx_min = nw.x;
    tx_max = se.x;
    ty_min = nw.y;
    ty_max = se.y;
  }

  int const tiles_x = tx_max - tx_min + 1;
  int const tiles_y = ty_max - ty_min + 1;
  uint32_t const total_w = static_cast<uint32_t>(tiles_x * 256);
  uint32_t const total_h = static_cast<uint32_t>(tiles_y * 256);

  MBASE_LOG_INFO(
    "OsmMinimap::Prepare: bbox lat=[{:.5f},{:.5f}] lng=[{:.5f},{:.5f}] "
    "zoom={} tiles={}x{} ({}x{} px)",
    lat_min, lat_max, lng_min, lng_max,
    zoom, tiles_x, tiles_y, total_w, total_h);

#if defined(_WIN32)
  std::wstring const wagent = Utf8ToWide(config.user_agent);
#endif

  std::vector<uint8_t> rgba(static_cast<size_t>(total_w) * total_h * 4, 0);

  auto const cache_root = std::filesystem::path(config.cache_dir);

  int fetched_count = 0;
  for (int ty = ty_min; ty <= ty_max; ++ty) {
    for (int tx = tx_min; tx <= tx_max; ++tx) {
      auto const tile_path = cache_root
        / std::to_string(zoom)
        / std::to_string(tx)
        / (std::to_string(ty) + ".png");

      std::vector<uint8_t> png_bytes;
      bool const from_cache = ReadFileBytes(tile_path, png_bytes);
      if (!from_cache) {
#if defined(_WIN32)
        char url_path[64];
        std::snprintf(url_path, sizeof(url_path),
                      "/%d/%d/%d.png", zoom, tx, ty);
        auto const wurl = Utf8ToWide(url_path);
        int status = 0;
        if (!WinHttpGet(L"tile.openstreetmap.org", wurl.c_str(),
                        wagent.c_str(), png_bytes, status)) {
          MBASE_LOG_ERROR("OsmMinimap: HTTP GET {} failed status={}",
                          url_path, status);
          return nullptr;
        }
        if (!WriteFileBytes(tile_path, png_bytes.data(), png_bytes.size())) {
          MBASE_LOG_WARN("OsmMinimap: cache write failed for {}",
                         tile_path.string());
        }
        ++fetched_count;
#else
        MBASE_LOG_ERROR("OsmMinimap: HTTP fetch not implemented on this platform");
        return nullptr;
#endif
      }

      std::optional<mimage::DecodedImage> decoded = mimage::DecodeImage(
        reinterpret_cast<std::byte const*>(png_bytes.data()),
        static_cast<uint32_t>(png_bytes.size()));
      if (!decoded.has_value() || decoded->width != 256 || decoded->height != 256) {
        MBASE_LOG_ERROR("OsmMinimap: PNG decode failed for {}/{}/{}",
                        zoom, tx, ty);
        return nullptr;
      }

      int const col = tx - tx_min;
      int const row = ty - ty_min;
      for (int yy = 0; yy < 256; ++yy) {
        uint8_t* dst = rgba.data()
          + (static_cast<size_t>(row) * 256 + yy) * total_w * 4
          + static_cast<size_t>(col) * 256 * 4;
        std::byte const* src = decoded->rgba_pixels.data() + static_cast<size_t>(yy) * 256 * 4;
        std::memcpy(dst, src, 256 * 4);
      }
    }
  }

  MBASE_LOG_INFO("OsmMinimap::Prepare: stitched OK, fetched={} cached={}",
                 fetched_count, tiles_x * tiles_y - fetched_count);

  auto m = std::unique_ptr<OsmMinimap>(new OsmMinimap());
  m->impl_ = std::make_unique<Impl>();
  m->impl_->zoom   = zoom;
  m->impl_->tx_min = tx_min;
  m->impl_->tx_max = tx_max;
  m->impl_->ty_min = ty_min;
  m->impl_->ty_max = ty_max;
  m->impl_->width  = total_w;
  m->impl_->height = total_h;
  m->impl_->rgba   = std::move(rgba);
  return m;
}

OsmMinimap::~OsmMinimap() = default;

uint32_t OsmMinimap::base_map_width()  const { return impl_->width;  }
uint32_t OsmMinimap::base_map_height() const { return impl_->height; }
uint8_t const* OsmMinimap::base_map_rgba() const { return impl_->rgba.data(); }

bool OsmMinimap::ProjectGpsToPixel(double lat, double lng,
                                   float& out_x, float& out_y) const {
  double tx_f = 0.0;
  double ty_f = 0.0;
  LatLngToTileF(lat, lng, impl_->zoom, tx_f, ty_f);
  double const dx = (tx_f - static_cast<double>(impl_->tx_min)) * 256.0;
  double const dy = (ty_f - static_cast<double>(impl_->ty_min)) * 256.0;
  out_x = static_cast<float>(dx);
  out_y = static_cast<float>(dy);
  return dx >= 0.0 && dx < static_cast<double>(impl_->width)
      && dy >= 0.0 && dy < static_cast<double>(impl_->height);
}

} // namespace routecam
