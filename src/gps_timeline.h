#pragma once

// c++ system headers -----------------------------------
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// public project headers --------------------------------
#include "mgpmf/public/gpmf_sample.h"

namespace routecam {

/// One GPS fix pinned to the media timeline.
struct GpsPoint final {
  /// Media-timeline time of the fix in seconds (the owning `gpmd`
  /// sample's CTS converted with the track timescale). Comparable
  /// with `mplay::MediaPlayer::current_pts_seconds()`.
  double      pts_seconds = 0.0;
  mgpmf::Gps9 gps;
};

/// GPS telemetry of one GoPro MP4: every GPS9 fix from the GPMF
/// (`gpmd`) track, extracted once at load time and pinned to the
/// media timeline. Foundation for the speed HUD, the map overlay
/// (track polyline), and the burn-in transcoder.
class GpsTimeline final {
public:
  /// Opens `mp4_path`'s GPMF track and collects all GPS9 fixes with
  /// a 2D-or-better lock. Returns `nullptr` when the file has no
  /// GPMF track or no usable fix (non-GPS GoPro content, indoor
  /// captures, non-GoPro MP4s).
  static std::unique_ptr<GpsTimeline> Load(std::string const& mp4_path);

  /// All fixes in ascending `pts_seconds` order.
  std::span<GpsPoint const> points() const { return points_; }

  /// Interpolated fix at `pts_seconds`. Linear between the two
  /// neighbouring fixes. Neighbours further apart than a GPS-dropout
  /// threshold are not interpolated across -- the nearer fix is
  /// returned as-is instead. Outside the covered range the boundary
  /// fix is returned only within that same threshold; beyond it
  /// (e.g. the pre-lock lead-in at the start of a capture) the
  /// result is `nullopt` and the caller should hide its display.
  std::optional<mgpmf::Gps9> SampleAt(double pts_seconds) const;

private:
  GpsTimeline() = default;

  std::vector<GpsPoint> points_;
};

} // namespace routecam
