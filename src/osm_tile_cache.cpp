// TU header --------------------------------------------
#include "osm_tile_cache.h"

// platform detection headers ---------------------------
#include "mbase/public/platform.h"

// c++ system headers -----------------------------------
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// external headers --------------------------------------
#if MBASE_PLATFORM_WINDOWS
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <windows.h>
# include <winhttp.h>
#endif

// public project headers --------------------------------
#include "mbase/public/log.h"
#include "mimage/public/mimage.h"
#include "mnexus/public/mnexus.h"

namespace routecam {

namespace {

// Identifying User-Agent per the OSM tile usage policy.
constexpr wchar_t const* kUserAgent = L"RouteCam/0.1 (personal action-cam viewer)";
constexpr wchar_t const* kTileHost  = L"tile.openstreetmap.org";

/// Blocking HTTPS GET of one tile. Returns the PNG bytes, or empty
/// on any failure (non-200, network error). Runs on the worker
/// thread only.
std::vector<std::byte> HttpGetTile(TileCoord coord) {
  std::vector<std::byte> bytes;
#if MBASE_PLATFORM_WINDOWS
  HINTERNET session = WinHttpOpen(
    kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) return bytes;

  HINTERNET connect = WinHttpConnect(session, kTileHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (connect != nullptr) {
    wchar_t path[64];
    std::swprintf(path, 64, L"/%d/%d/%d.png", coord.z, coord.x, coord.y);
    HINTERNET request = WinHttpOpenRequest(
      connect, L"GET", path, nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request != nullptr) {
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        WinHttpQueryHeaders(
          request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
          WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
          DWORD available = 0;
          while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            size_t const offset = bytes.size();
            bytes.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, bytes.data() + offset, available, &read)) {
              bytes.clear();
              break;
            }
            bytes.resize(offset + read);
          }
        } else {
          MBASE_LOG_WARN("OsmTileCache: HTTP {} for tile {}/{}/{}",
                         status, coord.z, coord.x, coord.y);
        }
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connect);
  }
  WinHttpCloseHandle(session);
#else
  // No network fetch on this platform yet; disk cache only.
  (void)coord;
#endif
  return bytes;
}

} // namespace

struct OsmTileCache::Impl final {
  enum class State { kLoading, kReady, kFailed };

  mnexus::IDevice*      device = nullptr;
  std::filesystem::path cache_dir;

  // Owner-thread state: per-tile lifecycle + resident textures.
  std::unordered_map<TileCoord, State, TileCoordHash>                 states;
  std::unordered_map<TileCoord, mnexus::TextureHandle, TileCoordHash> textures;

  // Worker plumbing.
  struct DecodedTile final {
    TileCoord            coord;
    bool                 ok = false;
    mimage::DecodedImage image;
  };
  std::mutex              mutex;
  std::condition_variable cv;
  std::deque<TileCoord>   requests;    // guarded by mutex
  std::deque<DecodedTile> completed;   // guarded by mutex
  std::atomic<bool>       quit{false};
  std::thread             worker;

  void WorkerMain() {
    for (;;) {
      TileCoord coord;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return quit.load() || !requests.empty(); });
        if (quit.load()) return;
        coord = requests.front();
        requests.pop_front();
      }

      DecodedTile result{ .coord = coord };

      // Disk cache first; network on miss (then persist).
      std::filesystem::path const tile_path =
        cache_dir / std::to_string(coord.z) / std::to_string(coord.x) /
        (std::to_string(coord.y) + ".png");

      std::vector<std::byte> png_bytes;
      {
        std::ifstream file(tile_path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
          auto const size = file.tellg();
          png_bytes.resize(static_cast<size_t>(size));
          file.seekg(0);
          file.read(reinterpret_cast<char*>(png_bytes.data()), size);
        }
      }
      if (png_bytes.empty()) {
        png_bytes = HttpGetTile(coord);
        if (!png_bytes.empty()) {
          std::error_code ec;
          std::filesystem::create_directories(tile_path.parent_path(), ec);
          std::ofstream file(tile_path, std::ios::binary | std::ios::trunc);
          file.write(reinterpret_cast<char const*>(png_bytes.data()),
                     static_cast<std::streamsize>(png_bytes.size()));
        }
      }

      if (!png_bytes.empty()) {
        std::optional<mimage::DecodedImage> decoded = mimage::DecodeImage(
          png_bytes.data(), static_cast<uint32_t>(png_bytes.size()));
        if (decoded.has_value()) {
          result.ok    = true;
          result.image = std::move(*decoded);
        }
      }
      if (!result.ok) {
        MBASE_LOG_WARN("OsmTileCache: failed to fetch/decode tile {}/{}/{}",
                       coord.z, coord.x, coord.y);
      }

      {
        std::lock_guard lock(mutex);
        completed.push_back(std::move(result));
      }
    }
  }
};

std::unique_ptr<OsmTileCache> OsmTileCache::Create(
    mnexus::IDevice* device, std::string cache_dir) {
  auto cache = std::unique_ptr<OsmTileCache>(new OsmTileCache());
  cache->impl_->device    = device;
  cache->impl_->cache_dir = std::filesystem::path(std::move(cache_dir));
  cache->impl_->worker    = std::thread([impl = cache->impl_.get()] {
    impl->WorkerMain();
  });
  MBASE_LOG_INFO("OsmTileCache: cache dir {}", cache->impl_->cache_dir.string());
  return cache;
}

OsmTileCache::OsmTileCache() : impl_(std::make_unique<Impl>()) {}

OsmTileCache::~OsmTileCache() {
  impl_->quit.store(true);
  impl_->cv.notify_all();
  if (impl_->worker.joinable()) impl_->worker.join();
  for (auto const& [coord, texture] : impl_->textures) {
    impl_->device->DestroyTexture(texture);
  }
}

mnexus::TextureHandle OsmTileCache::GetTile(TileCoord coord) {
  auto const it = impl_->states.find(coord);
  if (it != impl_->states.end()) {
    if (it->second == Impl::State::kReady) return impl_->textures.at(coord);
    return {};  // still loading, or failed (no retry this session)
  }

  impl_->states.emplace(coord, Impl::State::kLoading);
  {
    std::lock_guard lock(impl_->mutex);
    impl_->requests.push_back(coord);
  }
  impl_->cv.notify_one();
  return {};
}

void OsmTileCache::Update() {
  std::deque<Impl::DecodedTile> completed;
  {
    std::lock_guard lock(impl_->mutex);
    completed.swap(impl_->completed);
  }

  for (Impl::DecodedTile& tile : completed) {
    if (!tile.ok) {
      impl_->states[tile.coord] = Impl::State::kFailed;
      continue;
    }

    mnexus::IDevice* const device = impl_->device;
    mnexus::TextureHandle const texture = device->CreateTexture(mnexus::TextureDesc{
      .usage  = mnexus::TextureUsageFlagBits::kSampled | mnexus::TextureUsageFlagBits::kTransferDst,
      .format = mnexus::Format::kR8G8B8A8_UNORM,
      .width  = tile.image.width,
      .height = tile.image.height,
    });

    // Staging upload, mirroring the ImGui font atlas path.
    uint32_t const data_size =
      tile.image.width * tile.image.height * 4;
    mnexus::BufferHandle const staging = device->CreateBuffer(mnexus::BufferDesc{
      .usage         = mnexus::BufferUsageFlagBits::kTransferSrc | mnexus::BufferUsageFlagBits::kTransferDst,
      .size_in_bytes = data_size,
    });
    device->QueueWriteBuffer({}, staging, 0, tile.image.rgba_pixels.data(), data_size);

    mnexus::ICommandList* const cmd = device->CreateCommandList(mnexus::CommandListDesc{});
    auto const range = mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0);
    cmd->TextureBarrier(
      texture, range,
      mnexus::ResourceBarrierStageFlagBits::kTransfer,
      mnexus::ResourceBarrierState::kTransferDst);
    cmd->CopyBufferToTexture(
      staging, 0, texture, range,
      mnexus::Extent3d{ tile.image.width, tile.image.height, 1 });
    cmd->TextureBarrier(
      texture, range,
      mnexus::ResourceBarrierStageFlagBits::kFragmentShader,
      mnexus::ResourceBarrierState::kReadOnly);
    cmd->End();

    mnexus::IntraQueueSubmissionId const submit_id = device->QueueSubmitCommandList({}, cmd);
    device->QueueWaitIdle({}, submit_id);
    device->DestroyBuffer(staging);

    impl_->textures[tile.coord] = texture;
    impl_->states[tile.coord]   = Impl::State::kReady;
  }
}

} // namespace routecam
