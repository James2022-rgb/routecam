// TU header --------------------------------------------
#include "gps_timeline.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <cstdint>

// public project headers --------------------------------
#include "mbase/public/log.h"
#include "mdemux/public/mp4_gpmf_track_demuxer.h"

namespace routecam {

namespace {

// GoPro GPS9 fix codes: 0 = no lock, 2 = 2D lock, 3 = 3D lock.
// Fixes below 2D lock carry garbage coordinates.
constexpr uint32_t kMinFixCode = 2;

// Adjacent fixes further apart than this are a GPS dropout (tunnel,
// indoors); interpolating across the gap would smear stale motion
// over it, so `SampleAt` snaps to the nearer fix instead.
constexpr double kMaxLerpGapSeconds = 3.0;

float Lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

} // namespace

std::unique_ptr<GpsTimeline> GpsTimeline::Load(std::string const& mp4_path) {
  auto demuxer = mdemux::Mp4GpmfTrackDemuxer::Open(mp4_path);
  if (demuxer == nullptr) {
    MBASE_LOG_INFO("GpsTimeline::Load: no GPMF track in {}", mp4_path);
    return nullptr;
  }

  double const timescale = static_cast<double>(demuxer->timescale());
  uint32_t const total   = demuxer->total_sample_count();

  auto timeline = std::unique_ptr<GpsTimeline>(new GpsTimeline());
  timeline->points_.reserve(total);

  uint32_t parse_failures = 0;
  uint32_t weak_fixes     = 0;
  for (uint32_t sample_no = 1; sample_no <= total; ++sample_no) {
    auto const sample = demuxer->GetSampleByDecodeNo(sample_no);
    auto const parsed = mgpmf::GpmfSample::Parse(
      sample.data.data(), static_cast<uint32_t>(sample.data.size()));
    if (parsed == nullptr) {
      ++parse_failures;
      continue;
    }
    std::optional<mgpmf::Gps9> const gps = parsed->gps9();
    if (!gps.has_value()) continue;
    if (gps->fix < kMinFixCode) {
      ++weak_fixes;
      continue;
    }
    timeline->points_.push_back(GpsPoint{
      .pts_seconds = static_cast<double>(sample.cts) / timescale,
      .gps         = *gps,
    });
  }

  if (timeline->points_.empty()) {
    MBASE_LOG_INFO(
      "GpsTimeline::Load: GPMF track present but no usable GPS9 fix in {} "
      "({} samples, {} weak fixes, {} parse failures)",
      mp4_path, total, weak_fixes, parse_failures);
    return nullptr;
  }

  MBASE_LOG_INFO(
    "GpsTimeline::Load: {} fixes over [{:.2f}s, {:.2f}s] from {} gpmd samples "
    "({} weak fixes skipped, {} parse failures)",
    timeline->points_.size(),
    timeline->points_.front().pts_seconds,
    timeline->points_.back().pts_seconds,
    total, weak_fixes, parse_failures);
  return timeline;
}

std::optional<mgpmf::Gps9> GpsTimeline::SampleAt(double pts_seconds) const {
  if (points_.empty()) return std::nullopt;

  auto const after = std::lower_bound(
    points_.begin(), points_.end(), pts_seconds,
    [](GpsPoint const& p, double t) { return p.pts_seconds < t; });

  // Outside the covered range: clamp to the boundary fix, but only
  // within the dropout threshold. A capture's pre-lock lead-in (GPS
  // often locks tens of seconds in) shows no HUD rather than a
  // misleading frozen first fix.
  if (after == points_.begin()) {
    return (points_.front().pts_seconds - pts_seconds <= kMaxLerpGapSeconds)
             ? std::optional<mgpmf::Gps9>{points_.front().gps} : std::nullopt;
  }
  if (after == points_.end()) {
    return (pts_seconds - points_.back().pts_seconds <= kMaxLerpGapSeconds)
             ? std::optional<mgpmf::Gps9>{points_.back().gps} : std::nullopt;
  }

  GpsPoint const& lo = *(after - 1);
  GpsPoint const& hi = *after;

  double const gap = hi.pts_seconds - lo.pts_seconds;
  if (gap > kMaxLerpGapSeconds) {
    // GPS dropout: snap to the nearer endpoint.
    return (pts_seconds - lo.pts_seconds < hi.pts_seconds - pts_seconds)
             ? lo.gps : hi.gps;
  }

  float const t = gap > 0.0
    ? static_cast<float>((pts_seconds - lo.pts_seconds) / gap)
    : 0.0f;

  mgpmf::Gps9 out = lo.gps;  // fix / dop / UTC fields from the earlier fix
  out.latitude  = Lerp(lo.gps.latitude,  hi.gps.latitude,  t);
  out.longitude = Lerp(lo.gps.longitude, hi.gps.longitude, t);
  out.altitude  = Lerp(lo.gps.altitude,  hi.gps.altitude,  t);
  out.speed_2d  = Lerp(lo.gps.speed_2d,  hi.gps.speed_2d,  t);
  out.speed_3d  = Lerp(lo.gps.speed_3d,  hi.gps.speed_3d,  t);
  return out;
}

} // namespace routecam
