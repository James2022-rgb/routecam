#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>

namespace mnexus { class IDevice; }

namespace routecam {

/// Input to `TranscodeSession::Start`.
struct TranscodeDesc final {
  std::string input_path;
  std::string output_path;
  /// OSM tile disk cache (same layout / directory as `OsmTileCache`).
  std::string map_cache_dir;
  /// Encode-side downsample factor (1 = native, 2 = half, 4 =
  /// quarter). The dominant lever on encode throughput.
  uint32_t    encode_scale = 1;
};

/// One burn-in transcode run: decodes the source with mplay, draws
/// the telemetry dashboard (speedometer + GPS readouts + OSM route
/// maps) into an offscreen overlay, composes it over the video into
/// NV12 encode input, re-encodes with Vulkan Video (mhevcenc), and
/// muxes video + passthrough AAC / GPMF / tmcd tracks (mmux).
///
/// Owns its own `mplay::MediaPlayer` (audio disabled). The caller
/// MUST destroy any other player on the same file before `Start`
/// (NVDEC session limits) and drive `Tick` once per app frame while
/// `state() == kTranscoding`. Windows-only (Vulkan Video encode).
class TranscodeSession final {
public:
  enum class State { kTranscoding, kDone, kError };

  /// Opens the source, prepares the OSM route mosaics (synchronous
  /// tile fetch), allocates the encode pipeline, and opens the
  /// output muxer. Returns `nullptr` when the source cannot be
  /// opened or is unsupported (non-8-bit, no frames) or any
  /// pipeline component fails to initialize.
  static std::unique_ptr<TranscodeSession> Start(
    mnexus::IDevice* device, TranscodeDesc desc);

  /// Drains in-flight work and abandons the output when still
  /// transcoding.
  ~TranscodeSession();
  TranscodeSession(TranscodeSession const&) = delete;
  TranscodeSession& operator=(TranscodeSession const&) = delete;

  /// Decode + compose + encode + mux one frame. No-op unless
  /// `state() == kTranscoding`. Call once per app frame.
  void Tick();

  /// Aborts the run (output file is abandoned).
  void Cancel();

  State state() const;
  std::string const& last_error() const;

  // Progress readouts for the UI.
  uint32_t next_frame() const;
  uint32_t total_frames() const;
  uint32_t encoded_irap_count() const;
  uint32_t encoded_p_count() const;
  uint64_t encoded_bytes() const;
  double   elapsed_seconds() const;
  double   source_duration_seconds() const;
  uint32_t encode_width() const;
  uint32_t encode_height() const;
  std::string const& output_path() const;

  /// Rolling per-stage average latencies in milliseconds.
  struct StageAverages final {
    double decode_ms = 0.0;
    double gfx_ms    = 0.0;
    double wait_ms   = 0.0;
    double submit_ms = 0.0;
    double mux_ms    = 0.0;
  };
  StageAverages stage_averages() const;

private:
  TranscodeSession();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routecam
