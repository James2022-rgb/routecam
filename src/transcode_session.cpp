// TU header --------------------------------------------
#include "transcode_session.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

// external headers --------------------------------------
#include "imgui.h"

// public project headers --------------------------------
#include "masset/public/masset.h"
#include "mbase/public/assert.h"
#include "mbase/public/log.h"
#include "mdemux/public/mp4_aac_audio_demuxer.h"
#include "mdemux/public/mp4_gpmf_track_demuxer.h"
#include "mdemux/public/mp4_timecode_track_demuxer.h"
#include "mgpmf/public/gpmf_sample.h"
#include "mhevcenc/public/hevc_encode_session.h"
#include "mmux/public/mp4_muxer.h"
#include "mnexus/public/mnexus.h"
#include "mnexus/public/types.h"
#include "mplay/public/media_player.h"
#include "mshell/public/imgui_renderer.h"
#include "mslang_proxy/public/mslang_proxy.h"

// project headers ---------------------------------------
#include "osm_minimap.h"

namespace routecam {

namespace {

constexpr int8_t                  kEncodeQp       = 28;
constexpr uint32_t                kEncodeGopSize  = 30;
constexpr mnexus::VideoH265Level  kEncodeLevel    = mnexus::VideoH265Level::k4_1;

constexpr uint32_t                kVideoTimescale = 30000;
// Sample duration is derived per-source from total_seconds /
// total_frames rounded to integer ticks at kVideoTimescale
// (29.97 fps -> 1001, 30 fps -> 1000). Set in Initialize.

// AAC LC AU spans exactly 1024 PCM frames; constant per the codec
// and matches what the demuxer-side ASC defines.
constexpr uint32_t                kAacSamplesPerAu = 1024;

// Overlay RT dimensions + dest-picture placement. The overlay is
// drawn into its own RGBA8 render target via a secondary
// ImguiRenderer instance (sharing the primary's font atlas); the
// compose shaders then sample it at the right region of the dest
// picture.
constexpr uint32_t kOverlayWidth   = 1280;
constexpr uint32_t kOverlayHeight  = 320;
constexpr uint32_t kOverlayMarginX = 32;
constexpr uint32_t kOverlayMarginY = 32;

// Number of encode-input slots in the ping-pong. mhevcenc enforces
// at most one outstanding SubmitPicture, but the input texture can
// safely be overwritten as soon as that encode CL has read it. We
// fill slot N while encode CL N-1 is still consuming slot N-1.
constexpr uint32_t kEncodeInputCount = 2;

mnexus::TextureSubresourceRange ColorRange() {
  return mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0);
}

mnexus::TextureSubresourceRange PlaneRange(mnexus::TextureAspectFlagBits aspect) {
  return mnexus::TextureSubresourceRange{
    .aspect_mask       = aspect,
    .base_mip_level    = 0,
    .mip_level_count   = 1,
    .base_array_layer  = 0,
    .array_layer_count = 1,
  };
}

mnexus::TextureSubresourceRange BothPlanesRange() {
  return mnexus::TextureSubresourceRange{
    .aspect_mask       = mnexus::TextureAspectFlagBits::kPlane0
                       | mnexus::TextureAspectFlagBits::kPlane1,
    .base_mip_level    = 0,
    .mip_level_count   = 1,
    .base_array_layer  = 0,
    .array_layer_count = 1,
  };
}

mnexus::ShaderModuleHandle CompileSlangModule(
  mnexus::IDevice* device,
  char const*      asset_path,
  char const*      module_name,
  char const*      entry_point)
{
  masset::IAssetManager* asset_manager = masset::IAssetManager::Get();
  MBASE_ASSERT(asset_manager != nullptr);

  std::vector<std::byte> shader_bytes;
  uint64_t shader_timestamp = 0;
  MBASE_ASSERT(asset_manager->LoadAssetEx(asset_path, shader_bytes, shader_timestamp));

  std::string slang_code(reinterpret_cast<char const*>(shader_bytes.data()), shader_bytes.size());

  std::optional<std::vector<uint32_t>> opt_spirv = mslang_proxy::CompileSlangToSpirv(
    module_name, asset_path, slang_code.c_str(), nullptr, "GLSL_150", entry_point);
  MBASE_ASSERT_MSG(opt_spirv.has_value(), "slang compile failed");

  mnexus::ShaderModuleHandle h = device->CreateShaderModule(mnexus::ShaderModuleDesc{
    .source_language    = mnexus::ShaderSourceLanguage::kSpirV,
    .code_ptr           = reinterpret_cast<uint64_t>(opt_spirv->data()),
    .code_size_in_bytes = static_cast<uint32_t>(opt_spirv->size() * sizeof(uint32_t)),
  });
  MBASE_ASSERT(h.IsValid());
  return h;
}

// ---- 7-segment digit primitive ---------------------------
//
// Drawn via ImDrawList::AddRectFilled rather than scaled-up ImGui
// font glyphs so the central readout stays crisp at any size -- the
// atlas is baked at ~15 px and naive 3-4x text upscaling visibly
// blurs.
uint8_t SevenSegBits(int digit) {
  static const uint8_t k[10] = {
    0b0111111, // 0: abcdef
    0b0000110, // 1: bc
    0b1011011, // 2: abdeg
    0b1001111, // 3: abcdg
    0b1100110, // 4: bcfg
    0b1101101, // 5: acdfg
    0b1111101, // 6: acdefg
    0b0000111, // 7: abc
    0b1111111, // 8: abcdefg
    0b1101111, // 9: abcdfg
  };
  if (digit < 0 || digit > 9) return 0;
  return k[digit];
}

void DrawSevenSeg(ImDrawList* dl, ImVec2 p, float w, float h, int digit,
                  ImU32 col_on, ImU32 col_off) {
  uint8_t const segs = SevenSegBits(digit);
  float const t  = h * 0.12f;             // segment thickness
  float const vh = (h - 3.0f * t) * 0.5f; // vertical segment length
  float const hw = w - 2.0f * t;          // horizontal segment length

  auto seg = [&](ImVec2 a, ImVec2 b, bool on) {
    dl->AddRectFilled(a, b, on ? col_on : col_off, t * 0.35f);
  };
  // a: top horizontal
  seg(ImVec2(p.x + t,        p.y),
      ImVec2(p.x + t + hw,   p.y + t),                       (segs >> 0) & 1);
  // b: top-right vertical
  seg(ImVec2(p.x + w - t,    p.y + t),
      ImVec2(p.x + w,        p.y + t + vh),                  (segs >> 1) & 1);
  // c: bottom-right vertical
  seg(ImVec2(p.x + w - t,    p.y + t + vh + t),
      ImVec2(p.x + w,        p.y + t + vh + t + vh),         (segs >> 2) & 1);
  // d: bottom horizontal
  seg(ImVec2(p.x + t,        p.y + h - t),
      ImVec2(p.x + t + hw,   p.y + h),                       (segs >> 3) & 1);
  // e: bottom-left vertical
  seg(ImVec2(p.x,            p.y + t + vh + t),
      ImVec2(p.x + t,        p.y + t + vh + t + vh),         (segs >> 4) & 1);
  // f: top-left vertical
  seg(ImVec2(p.x,            p.y + t),
      ImVec2(p.x + t,        p.y + t + vh),                  (segs >> 5) & 1);
  // g: middle horizontal
  seg(ImVec2(p.x + t,        p.y + t + vh),
      ImVec2(p.x + t + hw,   p.y + t + vh + t),              (segs >> 6) & 1);
}

} // namespace

struct TranscodeSession::Impl final {
  // ---- Construction-time inputs ---------------------------
  mnexus::IDevice* device = nullptr;
  TranscodeDesc    desc;
  mnexus::QueueId  gfx_queue;
  mnexus::QueueId  encode_queue;

  // ---- Pipeline components --------------------------------
  std::unique_ptr<mplay::MediaPlayer>              player;
  std::unique_ptr<mdemux::Mp4GpmfTrackDemuxer>     gpmf_demux;
  std::unique_ptr<mdemux::Mp4AacAudioDemuxer>      aac_demux;
  std::unique_ptr<mdemux::Mp4TimecodeTrackDemuxer> tmcd_demux;
  std::unique_ptr<mhevcenc::HevcEncodeSession>     encode_session;
  std::unique_ptr<mmux::Mp4Muxer>                  muxer;

  mmux::Mp4Muxer::TrackId video_track_id = 0;
  mmux::Mp4Muxer::TrackId audio_track_id = 0;
  mmux::Mp4Muxer::TrackId gpmd_track_id  = 0;
  mmux::Mp4Muxer::TrackId tmcd_track_id  = 0;

  uint32_t next_aac_sample_no  = 1;
  uint32_t next_gpmd_sample_no = 1;
  uint32_t video_sample_duration = 0;

  // ---- GPMF cache -----------------------------------------
  std::unique_ptr<mgpmf::GpmfSample> cached_gpmf;
  uint32_t cached_sample_no       = 0;
  uint64_t cached_sample_cts      = 0;
  uint32_t cached_sample_duration = 0;

  // ---- OSM route mosaics ----------------------------------
  struct RouteEntry final {
    uint32_t gpmf_sample_no = 0;  // 1-indexed; matches cached_sample_no
    double   lat            = 0.0;
    double   lng            = 0.0;
  };
  std::vector<RouteEntry>     route;
  std::unique_ptr<OsmMinimap> osm_minimap;        // whole-route overview
  mnexus::TextureHandle       minimap_tex;
  std::unique_ptr<OsmMinimap> osm_minimap_local;  // street-level detail
  mnexus::TextureHandle       minimap_local_tex;

  // ---- Overlay rendering ----------------------------------
  std::unique_ptr<mshell::ImguiRenderer> overlay_renderer;
  ImDrawList* overlay_drawlist = nullptr;

  // ---- GPU resources --------------------------------------
  std::array<mnexus::TextureHandle, kEncodeInputCount> encode_inputs{};
  mnexus::TextureHandle      scratch_y;
  mnexus::TextureHandle      scratch_cbcr;
  mnexus::TextureHandle      overlay_rgba_tex;
  mnexus::BufferHandle       compose_ubo;
  mnexus::SamplerHandle      linear_sampler;
  mnexus::ShaderModuleHandle compose_y_vs;
  mnexus::ShaderModuleHandle compose_y_fs;
  mnexus::ShaderModuleHandle compose_cbcr_vs;
  mnexus::ShaderModuleHandle compose_cbcr_fs;
  mnexus::ProgramHandle      compose_y_program;
  mnexus::ProgramHandle      compose_cbcr_program;

  // ---- Encode pipelining ----------------------------------
  std::optional<mhevcenc::HevcEncodeSession::SubmissionToken> pending_token;
  uint32_t pending_frame_n = 0;

  // ---- Source geometry ------------------------------------
  uint32_t source_width            = 0;
  uint32_t source_height           = 0;
  uint32_t source_rotation_degrees = 0;
  uint32_t eff_width               = 0;
  uint32_t eff_height              = 0;
  uint32_t total_frames            = 0;
  uint32_t encode_width            = 0;
  uint32_t encode_height           = 0;

  // ---- Run state ------------------------------------------
  State    state              = State::kTranscoding;
  uint32_t next_encode_frame  = 0;
  uint32_t encoded_irap_count = 0;
  uint32_t encoded_p_count    = 0;
  uint64_t encoded_bytes      = 0;
  std::chrono::steady_clock::time_point start_time{};
  double      final_elapsed_secs = 0.0;
  std::string last_error;

  struct StageTiming final {
    double decode_ms_sum = 0.0;  size_t decode_ms_n = 0;
    double gfx_ms_sum    = 0.0;  size_t gfx_ms_n    = 0;
    double wait_ms_sum   = 0.0;  size_t wait_ms_n   = 0;
    double submit_ms_sum = 0.0;  size_t submit_ms_n = 0;
    double mux_ms_sum    = 0.0;  size_t mux_ms_n    = 0;
  };
  StageTiming timing{};

  ~Impl() {
    // Drain the in-flight encode before tearing the session down so
    // the session destructor doesn't have to clean up an orphaned
    // submission.
    if (pending_token.has_value() && encode_session != nullptr) {
      (void)encode_session->WaitAndReceive(*pending_token);
      pending_token.reset();
    }
    if (muxer != nullptr) {
      muxer->Close();
      muxer.reset();
    }
    encode_session.reset();
    tmcd_demux.reset();
    aac_demux.reset();
    gpmf_demux.reset();
    cached_gpmf.reset();
    osm_minimap.reset();
    osm_minimap_local.reset();
    route.clear();
    player.reset();

    if (overlay_drawlist != nullptr) {
      delete overlay_drawlist;
      overlay_drawlist = nullptr;
    }

    if (device != nullptr) {
      if (compose_y_program.IsValid())    device->DestroyProgram(compose_y_program);
      if (compose_cbcr_program.IsValid()) device->DestroyProgram(compose_cbcr_program);
      if (compose_y_vs.IsValid())         device->DestroyShaderModule(compose_y_vs);
      if (compose_y_fs.IsValid())         device->DestroyShaderModule(compose_y_fs);
      if (compose_cbcr_vs.IsValid())      device->DestroyShaderModule(compose_cbcr_vs);
      if (compose_cbcr_fs.IsValid())      device->DestroyShaderModule(compose_cbcr_fs);
      if (linear_sampler.IsValid())       device->DestroySampler(linear_sampler);
      if (compose_ubo.IsValid())          device->DestroyBuffer(compose_ubo);
      if (overlay_rgba_tex.IsValid())     device->DestroyTexture(overlay_rgba_tex);
      if (minimap_tex.IsValid())          device->DestroyTexture(minimap_tex);
      if (minimap_local_tex.IsValid())    device->DestroyTexture(minimap_local_tex);
      DestroyEncodeResources();
    }

    if (overlay_renderer != nullptr) {
      overlay_renderer->Finalize(device);
      overlay_renderer.reset();
    }
  }

  // ---- Initialization -------------------------------------

  bool Initialize(mnexus::IDevice* dev, TranscodeDesc in_desc) {
    device = dev;
    desc   = std::move(in_desc);

    mnexus::QueueSelection queue_selection;
    device->GetQueueSelection(queue_selection);
    gfx_queue = queue_selection.present_capable;
    if (!queue_selection.dedicated_video_encode.has_value()) {
      MBASE_LOG_ERROR("TranscodeSession: no dedicated video encode queue");
      return false;
    }
    encode_queue = queue_selection.dedicated_video_encode.value();

    // ---- Open source MP4 via mplay (audio off: the transcode
    // seeks frame-by-frame, which an audio-master clock would
    // fight) ----------------------------------------------------
    player = mplay::MediaPlayer::Open(device, mplay::OpenMp4Desc{
      .path                 = desc.input_path,
      .video_track_index    = 0,
      .enable_audio         = false,
      .enable_decode_timing = false,
    });
    if (player == nullptr) {
      MBASE_LOG_ERROR("TranscodeSession: failed to open {}", desc.input_path);
      return false;
    }
    auto const& info = player->info();
    source_width_set(info.width, info.height, info.rotation_degrees);
    total_frames = info.total_display_frames;

    if (info.bit_depth != 8) {
      MBASE_LOG_ERROR("TranscodeSession: source bit depth {} != 8 -- only Main 8-bit supported (HDR10 sources are not yet transcodable)",
        info.bit_depth);
      return false;
    }
    if (total_frames == 0u || info.total_seconds <= 0.0) {
      MBASE_LOG_ERROR("TranscodeSession: source has zero frames or duration");
      return false;
    }

    // ---- GPMF telemetry side-channel -----------------------
    gpmf_demux = mdemux::Mp4GpmfTrackDemuxer::Open(desc.input_path);
    if (gpmf_demux == nullptr) {
      MBASE_LOG_WARN("TranscodeSession: no GPMF track -- overlay will have no telemetry");
    }

    // ---- Secondary ImguiRenderer for the overlay -----------
    // claim_font_atlas=false: reuse the texture the app's primary
    // ImguiRenderer already set on io.Fonts.
    overlay_renderer = mshell::ImguiRenderer::Create();
    overlay_renderer->Initialize(device, /*claim_font_atlas=*/false);
    overlay_drawlist = new ImDrawList(ImGui::GetDrawListSharedData());

    // ---- Fixed GPU resources -------------------------------
    overlay_rgba_tex = device->CreateTexture(mnexus::TextureDesc{
      .usage             = mnexus::TextureUsageFlagBits::kAttachment
                         | mnexus::TextureUsageFlagBits::kSampled,
      .format            = mnexus::Format::kR8G8B8A8_UNORM,
      .dimension         = mnexus::TextureDimension::k2D,
      .width             = kOverlayWidth,
      .height            = kOverlayHeight,
      .depth             = 1,
      .mip_level_count   = 1,
      .array_layer_count = 1,
    });
    if (!overlay_rgba_tex.IsValid()) return false;

    // 3 vec4: rotation row0 + row1 + overlay rect.
    compose_ubo = device->CreateBuffer(mnexus::BufferDesc{
      .usage         = mnexus::BufferUsageFlagBits::kUniform,
      .size_in_bytes = 48,
    });
    if (!compose_ubo.IsValid()) return false;

    linear_sampler = device->CreateSampler(mnexus::SamplerDesc{
      .min_filter     = mnexus::Filter::kLinear,
      .mag_filter     = mnexus::Filter::kLinear,
      .mipmap_filter  = mnexus::Filter::kNearest,
      .address_mode_u = mnexus::AddressMode::kClampToEdge,
      .address_mode_v = mnexus::AddressMode::kClampToEdge,
      .address_mode_w = mnexus::AddressMode::kClampToEdge,
    });
    if (!linear_sampler.IsValid()) return false;

    compose_y_vs = CompileSlangModule(device, "compose_nv12_to_y.slang", "compose_y", "vertex_main");
    compose_y_fs = CompileSlangModule(device, "compose_nv12_to_y.slang", "compose_y", "fragment_main");
    {
      std::array<mnexus::ShaderModuleHandle, 2> mods{ compose_y_vs, compose_y_fs };
      compose_y_program = device->CreateProgram(mnexus::ProgramDesc{ .shader_modules = mods });
      if (!compose_y_program.IsValid()) return false;
    }
    compose_cbcr_vs = CompileSlangModule(device, "compose_nv12_to_cbcr.slang", "compose_cbcr", "vertex_main");
    compose_cbcr_fs = CompileSlangModule(device, "compose_nv12_to_cbcr.slang", "compose_cbcr", "fragment_main");
    {
      std::array<mnexus::ShaderModuleHandle, 2> mods{ compose_cbcr_vs, compose_cbcr_fs };
      compose_cbcr_program = device->CreateProgram(mnexus::ProgramDesc{ .shader_modules = mods });
      if (!compose_cbcr_program.IsValid()) return false;
    }

    player->SetAutoPlay(false);

    // ---- Encode dims + resources ---------------------------
    // Round down to a multiple of 16 to satisfy HEVC CTU + chroma
    // 4:2:0 alignment.
    auto align_down = [](uint32_t v, uint32_t a) { return (v / a) * a; };
    uint32_t const scale = desc.encode_scale != 0 ? desc.encode_scale : 1u;
    encode_width  = align_down(eff_width  / scale, 16);
    encode_height = align_down(eff_height / scale, 16);
    if (encode_width == 0u || encode_height == 0u) {
      MBASE_LOG_ERROR("TranscodeSession: encode dims collapse to zero at scale 1/{}", scale);
      return false;
    }
    MBASE_LOG_INFO("TranscodeSession: {} frames @ {}x{} (scale 1/{}) -> {}",
      total_frames, encode_width, encode_height, scale, desc.output_path);

    if (!AllocateEncodeResources()) {
      MBASE_LOG_ERROR("TranscodeSession: AllocateEncodeResources failed");
      return false;
    }

    // Synchronous: walks the entire GPMF track, then HTTP-fetches
    // any uncached OSM tiles.
    PrepareMinimap();

    mhevcenc::EncoderConfig encode_cfg{};
    encode_cfg.width         = encode_width;
    encode_cfg.height        = encode_height;
    encode_cfg.qp            = kEncodeQp;
    encode_cfg.gop_size      = kEncodeGopSize;
    encode_cfg.level         = kEncodeLevel;
    encode_cfg.quality_level = 0;
    encode_session = mhevcenc::HevcEncodeSession::Create(device, encode_cfg);
    if (encode_session == nullptr) {
      MBASE_LOG_ERROR("TranscodeSession: HevcEncodeSession::Create failed");
      return false;
    }

    // Passthrough demuxers; missing ones just skip that track.
    aac_demux = mdemux::Mp4AacAudioDemuxer::Open(desc.input_path);
    if (aac_demux == nullptr) {
      MBASE_LOG_WARN("TranscodeSession: no AAC audio track; output won't have audio");
    }
    tmcd_demux = mdemux::Mp4TimecodeTrackDemuxer::Open(desc.input_path);
    if (tmcd_demux == nullptr) {
      MBASE_LOG_WARN("TranscodeSession: no tmcd track; output won't have timecode");
    }

    video_sample_duration = static_cast<uint32_t>(
      (info.total_seconds * static_cast<double>(kVideoTimescale)) /
      static_cast<double>(total_frames) + 0.5);
    if (video_sample_duration == 0u) video_sample_duration = 1000u;

    muxer = mmux::Mp4Muxer::Open(desc.output_path, kVideoTimescale);
    if (muxer == nullptr) {
      MBASE_LOG_ERROR("TranscodeSession: Mp4Muxer::Open failed for {}", desc.output_path);
      return false;
    }

    // Track 1: HEVC video (the re-encoded one).
    mmux::Mp4Muxer::HevcVideoTrackConfig vcfg{};
    vcfg.width                   = encode_width;
    vcfg.height                  = encode_height;
    vcfg.timescale               = kVideoTimescale;
    vcfg.default_sample_duration = video_sample_duration;
    vcfg.vps_sps_pps_annex_b     = encode_session->vps_sps_pps_bytes();
    video_track_id = muxer->AddHevcVideoTrack(vcfg);
    if (video_track_id == 0) {
      MBASE_LOG_ERROR("TranscodeSession: AddHevcVideoTrack failed");
      return false;
    }

    // Track 2: AAC audio (passthrough).
    if (aac_demux != nullptr) {
      mmux::Mp4Muxer::AacAudioTrackConfig acfg{};
      acfg.timescale     = aac_demux->timescale();
      acfg.sample_rate   = aac_demux->sample_rate();
      acfg.channel_count = aac_demux->channel_count();
      acfg.asc_bytes     = aac_demux->GetAscBytes();
      audio_track_id     = muxer->AddAacAudioTrack(acfg);
      if (audio_track_id == 0) {
        MBASE_LOG_WARN("TranscodeSession: AddAacAudioTrack failed -- skipping audio");
      }
    }

    // Track 3: GPMF passthrough via raw sample-entry copy.
    if (gpmf_demux != nullptr) {
      constexpr uint32_t kFourccGpmd = ('g' << 24) | ('p' << 16) | ('m' << 8) | 'd';
      gpmd_track_id = muxer->AddPassthroughTrackByCodec(
        desc.input_path, kFourccGpmd, "mmux GPMF passthrough");
      if (gpmd_track_id == 0) {
        MBASE_LOG_WARN("TranscodeSession: GPMF passthrough add failed -- skipping");
      }
    }

    // Track 4: tmcd timecode passthrough.
    if (tmcd_demux != nullptr) {
      constexpr uint32_t kFourccTmcd = ('t' << 24) | ('m' << 16) | ('c' << 8) | 'd';
      tmcd_track_id = muxer->AddPassthroughTrackByCodec(
        desc.input_path, kFourccTmcd, "mmux tmcd passthrough");
      if (tmcd_track_id == 0) {
        MBASE_LOG_WARN("TranscodeSession: tmcd passthrough add failed -- skipping");
      }
    }
    if (tmcd_track_id != 0 && tmcd_demux != nullptr && tmcd_demux->total_sample_count() > 0) {
      auto s = tmcd_demux->GetSampleByDecodeNo(1);
      (void)muxer->AppendSample(tmcd_track_id,
        s.data.data(), static_cast<uint32_t>(s.data.size()),
        s.cts, s.duration, /*is_sync=*/true);
    }

    // One-time UBO upload: rotation matrix + overlay rect.
    {
      uint32_t const rot = source_rotation_degrees;
      float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
      switch (rot) {
        case   0: a =  1.0f; b =  0.0f; c =  0.0f; d =  1.0f; break;
        case  90: a =  0.0f; b = -1.0f; c =  1.0f; d =  0.0f; break;
        case 180: a = -1.0f; b =  0.0f; c =  0.0f; d = -1.0f; break;
        case 270: a =  0.0f; b =  1.0f; c = -1.0f; d =  0.0f; break;
        default:  break;
      }
      // Overlay rect normalised against the encode dims, with NO
      // clamping. If the rect extends past the picture (heavily
      // downscaled encode) the shader's `ov_uv > 1` check ignores
      // the off-picture portion -- the visible part stays 1:1 in
      // pixels, so circles stay circles.
      float const ov_min_x = static_cast<float>(kOverlayMarginX) / static_cast<float>(encode_width);
      float const ov_min_y = static_cast<float>(kOverlayMarginY) / static_cast<float>(encode_height);
      float const ov_max_x = static_cast<float>(kOverlayMarginX + kOverlayWidth)  / static_cast<float>(encode_width);
      float const ov_max_y = static_cast<float>(kOverlayMarginY + kOverlayHeight) / static_cast<float>(encode_height);
      float const ubo_data[12] = {
        a,        b,        0.0f,     0.0f,      // u_uv_rotation_row0
        c,        d,        0.0f,     0.0f,      // u_uv_rotation_row1
        ov_min_x, ov_min_y, ov_max_x, ov_max_y,  // u_overlay_rect
      };
      device->QueueWriteBuffer({}, compose_ubo, 0, ubo_data, sizeof(ubo_data));
    }

    start_time = std::chrono::steady_clock::now();
    state      = State::kTranscoding;
    return true;
  }

  void source_width_set(uint32_t w, uint32_t h, uint32_t rot) {
    source_width            = w;
    source_height           = h;
    source_rotation_degrees = rot;
    bool const swap_axes = (rot == 90 || rot == 270);
    eff_width  = swap_axes ? h : w;
    eff_height = swap_axes ? w : h;
  }

  bool AllocateEncodeResources() {
    DestroyEncodeResources();

    for (uint32_t i = 0; i < kEncodeInputCount; ++i) {
      encode_inputs[i] = device->CreateTexture(mnexus::TextureDesc{
        .usage             = mnexus::TextureUsageFlagBits::kVideoEncodeSrc
                           | mnexus::TextureUsageFlagBits::kTransferDst,
        .format            = mnexus::Format::kG8_B8R8_2PLANE_420_UNORM,
        .dimension         = mnexus::TextureDimension::k2D,
        .width             = encode_width,
        .height            = encode_height,
        .depth             = 1,
        .mip_level_count   = 1,
        .array_layer_count = 1,
      });
      if (!encode_inputs[i].IsValid()) return false;
    }

    scratch_y = device->CreateTexture(mnexus::TextureDesc{
      .usage             = mnexus::TextureUsageFlagBits::kAttachment
                         | mnexus::TextureUsageFlagBits::kTransferSrc,
      .format            = mnexus::Format::kR8_UNORM,
      .dimension         = mnexus::TextureDimension::k2D,
      .width             = encode_width,
      .height            = encode_height,
      .depth             = 1,
      .mip_level_count   = 1,
      .array_layer_count = 1,
    });
    if (!scratch_y.IsValid()) return false;

    scratch_cbcr = device->CreateTexture(mnexus::TextureDesc{
      .usage             = mnexus::TextureUsageFlagBits::kAttachment
                         | mnexus::TextureUsageFlagBits::kTransferSrc,
      .format            = mnexus::Format::kR8G8_UNORM,
      .dimension         = mnexus::TextureDimension::k2D,
      .width             = encode_width  / 2,
      .height            = encode_height / 2,
      .depth             = 1,
      .mip_level_count   = 1,
      .array_layer_count = 1,
    });
    if (!scratch_cbcr.IsValid()) return false;
    return true;
  }

  void DestroyEncodeResources() {
    if (device == nullptr) return;
    for (auto& ei : encode_inputs) {
      if (ei.IsValid()) { device->DestroyTexture(ei); ei = {}; }
    }
    if (scratch_cbcr.IsValid()) { device->DestroyTexture(scratch_cbcr); scratch_cbcr = {}; }
    if (scratch_y.IsValid())    { device->DestroyTexture(scratch_y);    scratch_y    = {}; }
  }

  // Creates a kSampled / kTransferDst RGBA8 texture, blits the CPU
  // pixels into it via a one-shot CL, and transitions to ReadOnly.
  mnexus::TextureHandle UploadRgba8ToTexture(uint8_t const* pixels,
                                             uint32_t w, uint32_t h) {
    mnexus::TextureHandle tex = device->CreateTexture(mnexus::TextureDesc{
      .usage  = mnexus::TextureUsageFlagBits::kSampled
              | mnexus::TextureUsageFlagBits::kTransferDst,
      .format = mnexus::Format::kR8G8B8A8_UNORM,
      .width  = w,
      .height = h,
    });
    if (!tex.IsValid()) return tex;

    uint32_t const data_size = w * h * 4;
    mnexus::BufferHandle staging = device->CreateBuffer(mnexus::BufferDesc{
      .usage         = mnexus::BufferUsageFlagBits::kTransferSrc
                     | mnexus::BufferUsageFlagBits::kTransferDst,
      .size_in_bytes = data_size,
    });
    device->QueueWriteBuffer({}, staging, 0, pixels, data_size);

    mnexus::ICommandList* cmd = device->CreateCommandList(mnexus::CommandListDesc{});
    auto const range = ColorRange();
    cmd->TextureBarrier(tex, range,
                        mnexus::ResourceBarrierStageFlagBits::kTransfer,
                        mnexus::ResourceBarrierState::kTransferDst);
    cmd->CopyBufferToTexture(staging, 0, tex, range,
                             mnexus::Extent3d{w, h, 1});
    cmd->TextureBarrier(tex, range,
                        mnexus::ResourceBarrierStageFlagBits::kFragmentShader,
                        mnexus::ResourceBarrierState::kReadOnly);
    cmd->End();
    auto sid = device->QueueSubmitCommandList({}, cmd);
    device->QueueWaitIdle({}, sid);
    device->DestroyBuffer(staging);
    return tex;
  }

  // Walk every GPMF sample, collect each one's GPS9 fix into
  // `route`, then fetch the OSM tiles covering the route's bounding
  // box (disk-cached). No-op when the source has no GPMF track or
  // no valid GPS9 fix.
  void PrepareMinimap() {
    if (gpmf_demux == nullptr) return;

    uint32_t const total = gpmf_demux->total_sample_count();
    std::vector<OsmMinimap::GpsPoint> route_points;
    route_points.reserve(total);
    route.reserve(total);
    for (uint32_t i = 1; i <= total; ++i) {
      auto s = gpmf_demux->GetSampleByDecodeNo(i);
      auto parsed = mgpmf::GpmfSample::Parse(s.data.data(),
                                             static_cast<uint32_t>(s.data.size()));
      if (parsed == nullptr) continue;
      auto gps = parsed->gps9();
      if (!gps) continue;
      route.push_back(RouteEntry{i, gps->latitude, gps->longitude});
      route_points.push_back({gps->latitude, gps->longitude});
    }
    if (route_points.empty()) {
      MBASE_LOG_WARN("TranscodeSession: no GPS9 samples -- skipping minimap");
      return;
    }

    // Overview mosaic -- whole route in a few low-zoom tiles.
    {
      OsmMinimap::PrepareConfig cfg{};
      cfg.route     = route_points;
      cfg.cache_dir = desc.map_cache_dir;
      osm_minimap   = OsmMinimap::Prepare(cfg);
      if (osm_minimap != nullptr) {
        minimap_tex = UploadRgba8ToTexture(osm_minimap->base_map_rgba(),
                                           osm_minimap->base_map_width(),
                                           osm_minimap->base_map_height());
        if (!minimap_tex.IsValid()) {
          MBASE_LOG_WARN("TranscodeSession: overview minimap upload failed");
          osm_minimap.reset();
        }
      } else {
        MBASE_LOG_WARN("TranscodeSession: overview OsmMinimap::Prepare failed");
      }
    }

    // Local-detail mosaic -- higher zoom capped at 16x16 tiles.
    {
      OsmMinimap::PrepareConfig cfg{};
      cfg.route       = std::move(route_points);
      cfg.cache_dir   = desc.map_cache_dir;
      cfg.min_zoom    = 14;
      cfg.max_zoom    = 17;
      cfg.max_tiles_x = 16;
      cfg.max_tiles_y = 16;
      osm_minimap_local = OsmMinimap::Prepare(cfg);
      if (osm_minimap_local != nullptr) {
        minimap_local_tex = UploadRgba8ToTexture(
          osm_minimap_local->base_map_rgba(),
          osm_minimap_local->base_map_width(),
          osm_minimap_local->base_map_height());
        if (!minimap_local_tex.IsValid()) {
          MBASE_LOG_WARN("TranscodeSession: local minimap upload failed");
          osm_minimap_local.reset();
        }
      } else {
        MBASE_LOG_WARN("TranscodeSession: local OsmMinimap::Prepare failed");
      }
    }
  }

  // ---- GPMF cache -----------------------------------------

  void RefreshGpmfForPts(double pts_seconds) {
    if (gpmf_demux == nullptr) return;
    uint64_t const target_ticks = static_cast<uint64_t>(
      pts_seconds * static_cast<double>(gpmf_demux->timescale()));

    if (cached_sample_no != 0) {
      bool const in_window =
        target_ticks >= cached_sample_cts &&
        (cached_sample_duration == 0u ||
         target_ticks <  cached_sample_cts + cached_sample_duration);
      if (in_window) return;
    }

    uint32_t const total = gpmf_demux->total_sample_count();
    for (uint32_t i = 1; i <= total; ++i) {
      auto s = gpmf_demux->GetSampleByDecodeNo(i);
      bool const is_last = (i == total);
      bool const in_window =
        target_ticks >= s.cts &&
        (is_last || target_ticks < s.cts + s.duration);
      if (!in_window) continue;
      cached_gpmf = mgpmf::GpmfSample::Parse(s.data.data(),
                                             static_cast<uint32_t>(s.data.size()));
      cached_sample_no       = i;
      cached_sample_cts      = s.cts;
      cached_sample_duration = s.duration;
      return;
    }
    cached_gpmf.reset();
    cached_sample_no       = 0;
    cached_sample_cts      = 0;
    cached_sample_duration = 0;
  }

  // ---- Per-frame ImDrawList build -------------------------

  void BuildOverlayDrawList() {
    ImDrawList* dl = overlay_drawlist;
    dl->_ResetForNewFrame();

    ImVec2 const ov_size(static_cast<float>(kOverlayWidth),
                         static_cast<float>(kOverlayHeight));
    // Visible horizontal extent on the encoded picture: the overlay
    // rect on dest is `[margin, margin + kOverlayWidth)`, truncated
    // to encode_width at the right when the encode is small. Lay
    // out the dashboard inside the truncated extent.
    float const visible_w = encode_width > kOverlayMarginX
      ? std::min<float>(static_cast<float>(kOverlayWidth),
                        static_cast<float>(encode_width) - kOverlayMarginX)
      : static_cast<float>(kOverlayWidth);

    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(visible_w, ov_size.y), false);
    dl->PushTextureID(ImGui::GetIO().Fonts->TexID);

    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(visible_w, ov_size.y),
                      IM_COL32(0, 0, 0, 160), 8.0f);

    ImU32 const kCWhite  = IM_COL32(255, 255, 255, 255);
    ImU32 const kCDim    = IM_COL32(180, 180, 180, 255);
    ImU32 const kCRimHi  = IM_COL32(170, 185, 200, 255);
    ImU32 const kCRimLo  = IM_COL32( 70,  85, 100, 255);
    ImU32 const kCFill   = IM_COL32( 80, 220, 120, 240);
    ImU32 const kCFillHi = IM_COL32(255, 110, 110, 240);
    ImU32 const kCFace   = IM_COL32( 15,  20,  30, 240);

    ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
    float const font_h = font->FontSize;
    float const line_h = font_h * 1.2f;

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Frame %u / %u", next_encode_frame, total_frames);
    dl->AddText(ImVec2(16.0f, 6.0f), kCDim, buf);

    // ---- Cockpit-style speedometer (center) ----
    ImVec2 const speedo_center(visible_w * 0.5f, ov_size.y * 0.5f);
    float const r_outer = ov_size.y * 0.47f;
    float const r_rim   = r_outer - 4.0f;
    float const r_track = r_outer - 16.0f;

    // 300 deg sweep, empty wedge at the bottom. ImGui PathArcTo uses
    // screen-space CW angles (0 = +X, pi/2 = down).
    constexpr float kSpeedoStart = 2.094395f;   // 2*pi/3  -- 7 o'clock
    constexpr float kSpeedoEnd   = 7.330382f;   // 7*pi/3  -- 5 o'clock
    constexpr float kSpeedoSweep = kSpeedoEnd - kSpeedoStart;  // 300 deg
    constexpr float kMaxKph      = 180.0f;
    constexpr float kRedlineKph  = 150.0f;

    dl->AddCircleFilled(speedo_center, r_outer, kCFace, 64);
    dl->AddCircle      (speedo_center, r_outer, kCRimHi, 64, 2.5f);
    dl->AddCircle      (speedo_center, r_outer - 12.0f, kCRimLo, 64, 1.0f);

    dl->PathArcTo(speedo_center, r_track, kSpeedoStart, kSpeedoEnd, 64);
    dl->PathStroke(IM_COL32(45, 55, 70, 220), false, 8.0f);

    constexpr float kMpsToKph = 3.6f;
    float kph = 0.0f;
    bool have_gps = false;
    if (cached_gpmf != nullptr) {
      if (auto gps = cached_gpmf->gps9()) {
        kph = gps->speed_2d * kMpsToKph;
        have_gps = true;
      }
    }

    if (kph > 0.0f) {
      float const t = std::min(kph / kMaxKph, 1.0f);
      float const fill_end = kSpeedoStart + t * kSpeedoSweep;
      bool  const redlined = kph >= kRedlineKph;
      dl->PathArcTo(speedo_center, r_track, kSpeedoStart, fill_end, 64);
      dl->PathStroke(redlined ? kCFillHi : kCFill, false, 8.0f);
    }

    for (int i = 0; i <= 18; ++i) {
      float const tick_kph = i * 10.0f;
      float const angle    = kSpeedoStart + (tick_kph / kMaxKph) * kSpeedoSweep;
      float const cos_a    = std::cos(angle);
      float const sin_a    = std::sin(angle);
      bool  const major    = (i % 2) == 0;
      float const tick_len = major ? 11.0f : 6.0f;
      ImVec2 const p_outer(speedo_center.x + r_rim * cos_a,
                           speedo_center.y + r_rim * sin_a);
      ImVec2 const p_inner(speedo_center.x + (r_rim - tick_len) * cos_a,
                           speedo_center.y + (r_rim - tick_len) * sin_a);
      dl->AddLine(p_inner, p_outer,
                  major ? kCWhite : IM_COL32(140, 150, 160, 200),
                  major ? 1.8f : 1.0f);
      if (major) {
        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d", static_cast<int>(tick_kph));
        ImVec2 const sz = font->CalcTextSizeA(font_h, FLT_MAX, 0.0f, lbl);
        ImVec2 const lp(speedo_center.x + (r_rim - tick_len - 12.0f) * cos_a - sz.x * 0.5f,
                        speedo_center.y + (r_rim - tick_len - 12.0f) * sin_a - sz.y * 0.5f);
        dl->AddText(lp, kCDim, lbl);
      }
    }

    int const kph_int = std::clamp(static_cast<int>(kph + 0.5f), 0, 999);
    char digits[4]{};
    std::snprintf(digits, sizeof(digits), "%3d", kph_int);

    float const digit_w   = r_outer * 0.30f;
    float const digit_h   = r_outer * 0.55f;
    float const digit_gap = digit_w * 0.20f;
    int const num_digits  = 3;
    float const total_w   = num_digits * digit_w + (num_digits - 1) * digit_gap;
    float const digits_y  = speedo_center.y - digit_h * 0.5f - r_outer * 0.05f;
    float const digits_x0 = speedo_center.x - total_w * 0.5f;

    ImU32 const seg_off = IM_COL32(255, 255, 255, 24);
    for (int i = 0; i < num_digits; ++i) {
      ImVec2 const dp(digits_x0 + i * (digit_w + digit_gap), digits_y);
      if (digits[i] == ' ') {
        DrawSevenSeg(dl, dp, digit_w, digit_h, 8, seg_off, seg_off);
      } else {
        DrawSevenSeg(dl, dp, digit_w, digit_h, digits[i] - '0', kCWhite, seg_off);
      }
    }

    char const* unit = "km/h";
    float const unit_h  = font_h * 1.2f;
    ImVec2 const unit_sz = font->CalcTextSizeA(unit_h, FLT_MAX, 0.0f, unit);
    dl->AddText(font, unit_h,
                ImVec2(speedo_center.x - unit_sz.x * 0.5f,
                       digits_y + digit_h + 6.0f),
                kCDim, unit);

    dl->AddCircleFilled(speedo_center, 5.0f, kCRimHi, 16);

    // ---- Left panel: GPS readouts ----
    float const left_x = 16.0f;
    float       left_y = 28.0f;
    dl->AddText(ImVec2(left_x, left_y), kCDim, "GPS"); left_y += line_h;
    if (have_gps && cached_gpmf != nullptr) {
      auto gps = cached_gpmf->gps9();
      std::snprintf(buf, sizeof(buf), "%9.5f", gps->latitude);
      dl->AddText(ImVec2(left_x, left_y), kCWhite, buf); left_y += line_h;
      std::snprintf(buf, sizeof(buf), "%9.5f", gps->longitude);
      dl->AddText(ImVec2(left_x, left_y), kCWhite, buf); left_y += line_h;
      std::snprintf(buf, sizeof(buf), "alt %.1f m", gps->altitude);
      dl->AddText(ImVec2(left_x, left_y), kCDim, buf); left_y += line_h;
      std::snprintf(buf, sizeof(buf), "fix %u  DOP %.2f", gps->fix, gps->dop);
      dl->AddText(ImVec2(left_x, left_y), kCDim, buf); left_y += line_h;
      std::snprintf(buf, sizeof(buf), "3D %.2f m/s", gps->speed_3d);
      dl->AddText(ImVec2(left_x, left_y), kCDim, buf);
    } else {
      dl->AddText(ImVec2(left_x, left_y), kCDim, "-- no fix --");
    }

    // ---- Right panel: OSM dual map (overview + local detail) ----
    float const right_size = ov_size.y - 32.0f;
    float const right_x0   = visible_w - right_size - 16.0f;
    float const right_y0   = 16.0f;
    float const overview_h = right_size * 0.40f;
    ImVec2 const ov_panel_min(right_x0,              right_y0);
    ImVec2 const ov_panel_max(right_x0 + right_size, right_y0 + overview_h);
    ImVec2 const lo_panel_min(right_x0,              right_y0 + overview_h + 4.0f);
    ImVec2 const lo_panel_max(right_x0 + right_size, right_y0 + right_size);

    ImU32 const kCRouteAll  = IM_COL32( 40, 110, 230, 180);
    ImU32 const kCRouteDone = IM_COL32(255, 110, 110, 240);

    // Where in `route` does the currently-displayed frame sit?
    size_t traversed = 0;
    for (auto const& e : route) {
      if (e.gpmf_sample_no > cached_sample_no) break;
      ++traversed;
    }

    auto draw_no_map_placeholder = [&](ImVec2 p_min, ImVec2 p_max) {
      dl->AddRectFilled(p_min, p_max, IM_COL32(25, 30, 40, 220), 4.0f);
      dl->AddRect(p_min, p_max, kCRimLo, 4.0f, 0, 1.5f);
      char const* msg = "-- no map --";
      ImVec2 const sz = font->CalcTextSizeA(font_h, FLT_MAX, 0.0f, msg);
      dl->AddText(ImVec2(p_min.x + ((p_max.x - p_min.x) - sz.x) * 0.5f,
                         p_min.y + ((p_max.y - p_min.y) - sz.y) * 0.5f),
                  kCDim, msg);
    };

    // Overview (top): whole route, uniform-scale letterboxed.
    if (osm_minimap != nullptr && minimap_tex.IsValid()) {
      float const panel_w = ov_panel_max.x - ov_panel_min.x;
      float const panel_h = ov_panel_max.y - ov_panel_min.y;
      float const base_w  = static_cast<float>(osm_minimap->base_map_width());
      float const base_h  = static_cast<float>(osm_minimap->base_map_height());
      float const scale   = std::min(panel_w / base_w, panel_h / base_h);
      float const draw_w  = base_w * scale;
      float const draw_h  = base_h * scale;
      float const draw_x0 = ov_panel_min.x + (panel_w - draw_w) * 0.5f;
      float const draw_y0 = ov_panel_min.y + (panel_h - draw_h) * 0.5f;
      ImVec2 const draw_min(draw_x0, draw_y0);
      ImVec2 const draw_max(draw_x0 + draw_w, draw_y0 + draw_h);

      dl->AddRectFilled(ov_panel_min, ov_panel_max, IM_COL32(15, 20, 30, 240), 4.0f);
      dl->AddImage(static_cast<ImTextureID>(minimap_tex.Get()),
                   draw_min, draw_max);
      dl->AddRect(ov_panel_min, ov_panel_max, kCRimHi, 4.0f, 0, 1.0f);

      std::vector<ImVec2> pts;
      pts.reserve(route.size());
      for (auto const& e : route) {
        float bx = 0.0f;
        float by = 0.0f;
        osm_minimap->ProjectGpsToPixel(e.lat, e.lng, bx, by);
        pts.push_back(ImVec2(draw_min.x + bx * scale,
                             draw_min.y + by * scale));
      }
      dl->PushClipRect(ov_panel_min, ov_panel_max, true);
      if (pts.size() >= 2) {
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                        kCRouteAll, 0, 1.5f);
      }
      if (traversed >= 2) {
        dl->AddPolyline(pts.data(), static_cast<int>(traversed),
                        kCRouteDone, 0, 2.0f);
      }
      if (traversed > 0) {
        ImVec2 const p = pts[traversed - 1];
        dl->AddCircleFilled(p, 4.0f, kCWhite,                12);
        dl->AddCircle      (p, 4.0f, IM_COL32(0, 0, 0, 220), 12, 1.0f);
      }
      dl->PopClipRect();
    } else {
      draw_no_map_placeholder(ov_panel_min, ov_panel_max);
    }

    // Local (bottom): viewport centred on the current fix.
    if (osm_minimap_local != nullptr && minimap_local_tex.IsValid() && !route.empty()) {
      float const panel_w = lo_panel_max.x - lo_panel_min.x;
      float const panel_h = lo_panel_max.y - lo_panel_min.y;
      float const base_w  = static_cast<float>(osm_minimap_local->base_map_width());
      float const base_h  = static_cast<float>(osm_minimap_local->base_map_height());

      size_t const cur_idx = traversed > 0 ? traversed - 1 : 0;
      RouteEntry const& cur = route[cur_idx];

      float cx = 0.0f;
      float cy = 0.0f;
      osm_minimap_local->ProjectGpsToPixel(cur.lat, cur.lng, cx, cy);

      float const vp_w = 384.0f;
      float const vp_h = vp_w * (panel_h / panel_w);
      float vp_x0 = std::clamp(cx - vp_w * 0.5f,
                               0.0f, std::max(0.0f, base_w - vp_w));
      float vp_y0 = std::clamp(cy - vp_h * 0.5f,
                               0.0f, std::max(0.0f, base_h - vp_h));
      float const u0 = vp_x0 / base_w;
      float const v0 = vp_y0 / base_h;
      float const u1 = std::min((vp_x0 + vp_w) / base_w, 1.0f);
      float const v1 = std::min((vp_y0 + vp_h) / base_h, 1.0f);

      dl->AddRectFilled(lo_panel_min, lo_panel_max, IM_COL32(15, 20, 30, 240), 4.0f);
      dl->AddImage(static_cast<ImTextureID>(minimap_local_tex.Get()),
                   lo_panel_min, lo_panel_max,
                   ImVec2(u0, v0), ImVec2(u1, v1));
      dl->AddRect(lo_panel_min, lo_panel_max, kCRimHi, 4.0f, 0, 1.0f);

      float const sx = panel_w / vp_w;
      float const sy = panel_h / vp_h;
      std::vector<ImVec2> lpts;
      lpts.reserve(route.size());
      for (auto const& e : route) {
        float mx = 0.0f;
        float my = 0.0f;
        osm_minimap_local->ProjectGpsToPixel(e.lat, e.lng, mx, my);
        lpts.push_back(ImVec2(lo_panel_min.x + (mx - vp_x0) * sx,
                              lo_panel_min.y + (my - vp_y0) * sy));
      }
      dl->PushClipRect(lo_panel_min, lo_panel_max, true);
      if (lpts.size() >= 2) {
        dl->AddPolyline(lpts.data(), static_cast<int>(lpts.size()),
                        kCRouteAll, 0, 2.0f);
      }
      if (traversed >= 2) {
        dl->AddPolyline(lpts.data(), static_cast<int>(traversed),
                        kCRouteDone, 0, 3.0f);
      }
      if (traversed > 0) {
        ImVec2 const p = lpts[traversed - 1];
        dl->AddCircleFilled(p, 7.0f, kCWhite,                16);
        dl->AddCircle      (p, 7.0f, IM_COL32(0, 0, 0, 220), 16, 1.5f);
      }
      dl->PopClipRect();
    } else {
      draw_no_map_placeholder(lo_panel_min, lo_panel_max);
    }

    dl->PopTextureID();
    dl->PopClipRect();
  }

  // ---- Per-frame work -------------------------------------

  void EncodeNextFrame() {
    MBASE_ASSERT(state == State::kTranscoding);
    if (next_encode_frame >= total_frames) {
      DrainAndFinish();
      return;
    }
    uint32_t const frame_n = next_encode_frame;
    uint32_t const cur_input_idx = frame_n % kEncodeInputCount;

    auto stage_clock = std::chrono::steady_clock::now();
    auto take_stage_ms = [&](double& accum, size_t& sample_count) {
      auto const now = std::chrono::steady_clock::now();
      double const ms = std::chrono::duration<double, std::milli>(now - stage_clock).count();
      accum += ms;
      sample_count += 1;
      stage_clock = now;
    };

    // ---- 1. mplay decode --------------------------------
    if (frame_n > 0) {
      player->SeekToDisplayIndex(frame_n);
    }
    player->Update();
    player->Render();
    take_stage_ms(timing.decode_ms_sum, timing.decode_ms_n);

    // ---- 2. Overlay ImDrawList for this frame's PTS ------
    RefreshGpmfForPts(player->current_pts_seconds());
    BuildOverlayDrawList();

    ImDrawData dd;
    dd.Clear();
    dd.DisplayPos       = ImVec2(0.0f, 0.0f);
    dd.DisplaySize      = ImVec2(static_cast<float>(kOverlayWidth),
                                 static_cast<float>(kOverlayHeight));
    dd.FramebufferScale = ImVec2(1.0f, 1.0f);
    dd.AddDrawList(overlay_drawlist);
    dd.Valid = true;

    overlay_renderer->UpdateGeometryBuffers(device, &dd);

    mnexus::TextureHandle const y_tex    = player->current_y_texture();
    mnexus::TextureHandle const cbcr_tex = player->current_cbcr_texture();

    // ---- 3. gfx CL: overlay RT + compose + copy + release ----
    mnexus::ICommandList* gfx_cl = device->CreateCommandList(
      mnexus::CommandListDesc{ .queue_family_index = gfx_queue.queue_family_index });

    // 3a. Pre-clear the overlay RT to (0,0,0,0). The renderer's own
    // pass uses LoadOp::kLoad, so without this the prior frame's
    // pixels would persist where the drawlist doesn't paint.
    {
      gfx_cl->TextureBarrier(overlay_rgba_tex, ColorRange(),
        mnexus::ResourceBarrierStageFlagBits::kColorAttachmentOutput,
        mnexus::ResourceBarrierState::kAttachment);
      mnexus::ClearValue clear{};
      clear.color.r = 0.0f; clear.color.g = 0.0f; clear.color.b = 0.0f; clear.color.a = 0.0f;
      mnexus::ColorAttachmentDesc const attach{
        .texture           = overlay_rgba_tex,
        .subresource_range = ColorRange(),
        .load_op           = mnexus::LoadOp::kClear,
        .store_op          = mnexus::StoreOp::kStore,
        .clear_value       = clear,
      };
      gfx_cl->BeginRenderPass(mnexus::RenderPassDesc{ .color_attachments = attach });
      gfx_cl->EndRenderPass();
    }

    // 3b. Draw the overlay on top of the cleared content.
    overlay_renderer->Render(gfx_cl, overlay_rgba_tex, &dd);

    // 3c. Make the overlay readable by the compose passes.
    gfx_cl->TextureBarrier(overlay_rgba_tex, ColorRange(),
      mnexus::ResourceBarrierStageFlagBits::kFragmentShader,
      mnexus::ResourceBarrierState::kReadOnly);

    // 3d. Compose Y plane -> scratch_y.
    gfx_cl->TextureBarrier(scratch_y, ColorRange(),
      mnexus::ResourceBarrierStageFlagBits::kColorAttachmentOutput,
      mnexus::ResourceBarrierState::kAttachment);
    {
      mnexus::ColorAttachmentDesc const attach{
        .texture           = scratch_y,
        .subresource_range = ColorRange(),
        .load_op           = mnexus::LoadOp::kDontCare,
        .store_op          = mnexus::StoreOp::kStore,
      };
      gfx_cl->BeginRenderPass(mnexus::RenderPassDesc{ .color_attachments = attach });
      gfx_cl->BindRenderProgram(compose_y_program);
      gfx_cl->BindSampledTexture(
        mnexus::BindingId{ .group = 0, .binding = 0, .array_element = 0 },
        y_tex, ColorRange());
      gfx_cl->BindSampledTexture(
        mnexus::BindingId{ .group = 0, .binding = 1, .array_element = 0 },
        overlay_rgba_tex, ColorRange());
      gfx_cl->BindSampler(
        mnexus::BindingId{ .group = 0, .binding = 2, .array_element = 0 },
        linear_sampler);
      gfx_cl->BindUniformBuffer(
        mnexus::BindingId{ .group = 0, .binding = 3, .array_element = 0 },
        compose_ubo, 0, 48);
      gfx_cl->Draw(6, 1, 0, 0);
      gfx_cl->EndRenderPass();
    }

    // 3e. Compose CbCr plane -> scratch_cbcr.
    gfx_cl->TextureBarrier(scratch_cbcr, ColorRange(),
      mnexus::ResourceBarrierStageFlagBits::kColorAttachmentOutput,
      mnexus::ResourceBarrierState::kAttachment);
    {
      mnexus::ColorAttachmentDesc const attach{
        .texture           = scratch_cbcr,
        .subresource_range = ColorRange(),
        .load_op           = mnexus::LoadOp::kDontCare,
        .store_op          = mnexus::StoreOp::kStore,
      };
      gfx_cl->BeginRenderPass(mnexus::RenderPassDesc{ .color_attachments = attach });
      gfx_cl->BindRenderProgram(compose_cbcr_program);
      gfx_cl->BindSampledTexture(
        mnexus::BindingId{ .group = 0, .binding = 0, .array_element = 0 },
        cbcr_tex, ColorRange());
      gfx_cl->BindSampledTexture(
        mnexus::BindingId{ .group = 0, .binding = 1, .array_element = 0 },
        overlay_rgba_tex, ColorRange());
      gfx_cl->BindSampler(
        mnexus::BindingId{ .group = 0, .binding = 2, .array_element = 0 },
        linear_sampler);
      gfx_cl->BindUniformBuffer(
        mnexus::BindingId{ .group = 0, .binding = 3, .array_element = 0 },
        compose_ubo, 0, 48);
      gfx_cl->Draw(6, 1, 0, 0);
      gfx_cl->EndRenderPass();
    }

    // 3f. Scratches -> kTransferSrc, encode_input[cur] -> kTransferDst.
    mnexus::TextureHandle const cur_input = encode_inputs[cur_input_idx];
    gfx_cl->TextureBarrier(scratch_y, ColorRange(),
      mnexus::ResourceBarrierStageFlagBits::kTransfer,
      mnexus::ResourceBarrierState::kTransferSrc);
    gfx_cl->TextureBarrier(scratch_cbcr, ColorRange(),
      mnexus::ResourceBarrierStageFlagBits::kTransfer,
      mnexus::ResourceBarrierState::kTransferSrc);
    gfx_cl->TextureBarrier(cur_input, BothPlanesRange(),
      mnexus::ResourceBarrierStageFlagBits::kTransfer,
      mnexus::ResourceBarrierState::kTransferDst);

    // 3g. Copy scratches into encode_input[cur] planes.
    gfx_cl->CopyTextureToTexture(
      scratch_y, ColorRange(),
      cur_input, PlaneRange(mnexus::TextureAspectFlagBits::kPlane0),
      mnexus::Extent3d{ encode_width, encode_height, 1 });
    gfx_cl->CopyTextureToTexture(
      scratch_cbcr, ColorRange(),
      cur_input, PlaneRange(mnexus::TextureAspectFlagBits::kPlane1),
      mnexus::Extent3d{ encode_width / 2, encode_height / 2, 1 });

    // 3h. Release encode_input[cur] to the encode queue.
    gfx_cl->TextureBarrierRelease(cur_input, BothPlanesRange(),
      mnexus::ResourceBarrierState::kVideoEncodeSrc,
      encode_queue);

    gfx_cl->End();
    mnexus::IntraQueueSubmissionId const gfx_id =
      device->QueueSubmitCommandList(gfx_queue, gfx_cl);
    take_stage_ms(timing.gfx_ms_sum, timing.gfx_ms_n);

    // 4. Drain the prior frame's encode (depth-1 pipeline). Must
    //    drain before submitting any new encode-queue CL -- see the
    //    playground PoC for the semaphore-ordering rationale.
    if (pending_token.has_value()) {
      auto encoded = encode_session->WaitAndReceive(*pending_token);
      uint32_t const finished_video_frame_n = pending_frame_n;
      pending_token.reset();
      if (!encoded.has_value()) {
        Abort("HevcEncodeSession::WaitAndReceive returned nullopt");
        return;
      }
      take_stage_ms(timing.wait_ms_sum, timing.wait_ms_n);
      // Interleave: audio + gpmd samples in / through the finished
      // video frame's window go in BEFORE the video AU itself.
      double const window_end_secs =
        (static_cast<double>(finished_video_frame_n + 1) * video_sample_duration)
        / static_cast<double>(kVideoTimescale);
      DrainAudioUntil(window_end_secs);
      DrainGpmfUntil(window_end_secs);

      uint64_t const video_dts = static_cast<uint64_t>(finished_video_frame_n) * video_sample_duration;
      if (!muxer->AppendHevcAccessUnit(video_track_id,
                                       encoded->au_bytes.data(),
                                       static_cast<uint32_t>(encoded->au_bytes.size()),
                                       video_dts, video_sample_duration,
                                       encoded->is_irap)) {
        Abort("Mp4Muxer::AppendHevcAccessUnit failed");
        return;
      }
      (encoded->is_irap ? encoded_irap_count : encoded_p_count) += 1;
      encoded_bytes += encoded->au_bytes.size();
      take_stage_ms(timing.mux_ms_sum, timing.mux_ms_n);
    }

    // 5. Encode-queue acquire for cur_input.
    mnexus::ICommandList* enc_acq_cl = device->CreateCommandList(
      mnexus::CommandListDesc{ .queue_family_index = encode_queue.queue_family_index });
    enc_acq_cl->TextureBarrierAcquire(cur_input, BothPlanesRange(),
      mnexus::ResourceBarrierStageFlagBits::kVideoEncode,
      mnexus::ResourceBarrierState::kTransferDst,
      mnexus::ResourceBarrierState::kVideoEncodeSrc,
      gfx_queue);
    enc_acq_cl->End();
    std::array<mnexus::QueueWaitInfo, 1> const acq_waits{
      mnexus::QueueWaitInfo{ gfx_queue, gfx_id },
    };
    device->QueueSubmitCommandListWithWaits(encode_queue, enc_acq_cl, acq_waits);

    // 6. Submit encode for this frame.
    auto token = encode_session->SubmitPicture(cur_input, /*src_array_layer=*/0);
    if (!token.has_value()) {
      Abort("HevcEncodeSession::SubmitPicture returned nullopt");
      return;
    }
    pending_token   = token;
    pending_frame_n = frame_n;
    take_stage_ms(timing.submit_ms_sum, timing.submit_ms_n);

    next_encode_frame = frame_n + 1;
  }

  void DrainAndFinish() {
    MBASE_ASSERT(state == State::kTranscoding);
    if (pending_token.has_value()) {
      auto encoded = encode_session->WaitAndReceive(*pending_token);
      uint32_t const finished_video_frame_n = pending_frame_n;
      pending_token.reset();
      if (encoded.has_value()) {
        double const window_end_secs =
          (static_cast<double>(finished_video_frame_n + 1) * video_sample_duration)
          / static_cast<double>(kVideoTimescale);
        DrainAudioUntil(window_end_secs);
        DrainGpmfUntil(window_end_secs);

        uint64_t const video_dts = static_cast<uint64_t>(finished_video_frame_n) * video_sample_duration;
        if (!muxer->AppendHevcAccessUnit(video_track_id,
                                         encoded->au_bytes.data(),
                                         static_cast<uint32_t>(encoded->au_bytes.size()),
                                         video_dts, video_sample_duration,
                                         encoded->is_irap)) {
          Abort("Mp4Muxer::AppendHevcAccessUnit failed (final drain)");
          return;
        }
        (encoded->is_irap ? encoded_irap_count : encoded_p_count) += 1;
        encoded_bytes += encoded->au_bytes.size();
      } else {
        MBASE_LOG_ERROR("TranscodeSession: final WaitAndReceive returned nullopt; output may be incomplete");
      }
    }
    // Leftover audio / gpmd samples past the last video frame's
    // window (audio is usually a few AUs longer than video).
    DrainAudioUntil(/*until_secs=*/1e18);
    DrainGpmfUntil(/*until_secs=*/1e18);

    if (muxer != nullptr) {
      if (!muxer->Close()) {
        Abort("Mp4Muxer::Close failed");
        return;
      }
      muxer.reset();
    }
    final_elapsed_secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    MBASE_LOG_INFO("TranscodeSession: done -- {} frames ({} IRAP, {} P), {} bytes -> {}",
      next_encode_frame, encoded_irap_count, encoded_p_count, encoded_bytes,
      desc.output_path);
    state = State::kDone;
  }

  void DrainAudioUntil(double until_secs) {
    if (audio_track_id == 0 || aac_demux == nullptr) return;
    uint32_t const total = aac_demux->total_sample_count();
    uint32_t const ts    = aac_demux->sample_rate();  // == timescale
    if (ts == 0) return;
    while (next_aac_sample_no <= total) {
      uint64_t const dts_ticks = static_cast<uint64_t>(next_aac_sample_no - 1) * kAacSamplesPerAu;
      double const dts_secs = static_cast<double>(dts_ticks) / static_cast<double>(ts);
      if (dts_secs >= until_secs) break;
      auto s = aac_demux->GetSampleByDecodeNo(next_aac_sample_no);
      (void)muxer->AppendSample(audio_track_id,
        s.data.data(), static_cast<uint32_t>(s.data.size()),
        dts_ticks, kAacSamplesPerAu, /*is_sync=*/true);
      ++next_aac_sample_no;
    }
  }

  void DrainGpmfUntil(double until_secs) {
    if (gpmd_track_id == 0 || gpmf_demux == nullptr) return;
    uint32_t const total = gpmf_demux->total_sample_count();
    uint32_t const ts    = gpmf_demux->timescale();
    if (ts == 0) return;
    while (next_gpmd_sample_no <= total) {
      auto s = gpmf_demux->GetSampleByDecodeNo(next_gpmd_sample_no);
      double const cts_secs = static_cast<double>(s.cts) / static_cast<double>(ts);
      if (cts_secs >= until_secs) break;
      (void)muxer->AppendSample(gpmd_track_id,
        s.data.data(), static_cast<uint32_t>(s.data.size()),
        s.cts, s.duration, /*is_sync=*/true);
      ++next_gpmd_sample_no;
    }
  }

  void Abort(std::string reason) {
    MBASE_LOG_ERROR("TranscodeSession: aborted at frame {}: {}", next_encode_frame, reason);
    // Drain any in-flight encode so the session can be safely
    // released. The output file is abandoned.
    if (pending_token.has_value() && encode_session != nullptr) {
      (void)encode_session->WaitAndReceive(*pending_token);
      pending_token.reset();
    }
    if (muxer != nullptr) {
      muxer->Close();
      muxer.reset();
    }
    final_elapsed_secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    last_error = std::move(reason);
    state      = State::kError;
  }
};

// ---- Public wrappers ---------------------------------------

std::unique_ptr<TranscodeSession> TranscodeSession::Start(
    mnexus::IDevice* device, TranscodeDesc desc) {
  auto session = std::unique_ptr<TranscodeSession>(new TranscodeSession());
  if (!session->impl_->Initialize(device, std::move(desc))) {
    return nullptr;
  }
  return session;
}

TranscodeSession::TranscodeSession() : impl_(std::make_unique<Impl>()) {}
TranscodeSession::~TranscodeSession() = default;

void TranscodeSession::Tick() {
  if (impl_->state != State::kTranscoding) return;
  impl_->EncodeNextFrame();
}

void TranscodeSession::Cancel() {
  if (impl_->state != State::kTranscoding) return;
  impl_->Abort("cancelled by user");
}

TranscodeSession::State TranscodeSession::state() const { return impl_->state; }
std::string const& TranscodeSession::last_error() const { return impl_->last_error; }

uint32_t TranscodeSession::next_frame() const         { return impl_->next_encode_frame; }
uint32_t TranscodeSession::total_frames() const       { return impl_->total_frames; }
uint32_t TranscodeSession::encoded_irap_count() const { return impl_->encoded_irap_count; }
uint32_t TranscodeSession::encoded_p_count() const    { return impl_->encoded_p_count; }
uint64_t TranscodeSession::encoded_bytes() const      { return impl_->encoded_bytes; }
uint32_t TranscodeSession::encode_width() const       { return impl_->encode_width; }
uint32_t TranscodeSession::encode_height() const      { return impl_->encode_height; }
std::string const& TranscodeSession::output_path() const { return impl_->desc.output_path; }

double TranscodeSession::elapsed_seconds() const {
  if (impl_->state != State::kTranscoding) return impl_->final_elapsed_secs;
  return std::chrono::duration<double>(
    std::chrono::steady_clock::now() - impl_->start_time).count();
}

double TranscodeSession::source_duration_seconds() const {
  return impl_->player != nullptr ? impl_->player->info().total_seconds : 0.0;
}

TranscodeSession::StageAverages TranscodeSession::stage_averages() const {
  auto avg = [](double sum, size_t n) { return n > 0u ? sum / static_cast<double>(n) : 0.0; };
  auto const& t = impl_->timing;
  return StageAverages{
    .decode_ms = avg(t.decode_ms_sum, t.decode_ms_n),
    .gfx_ms    = avg(t.gfx_ms_sum,    t.gfx_ms_n),
    .wait_ms   = avg(t.wait_ms_sum,   t.wait_ms_n),
    .submit_ms = avg(t.submit_ms_sum, t.submit_ms_n),
    .mux_ms    = avg(t.mux_ms_sum,    t.mux_ms_n),
  };
}

} // namespace routecam
