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
#include "mdemux/public/mp4_hevc_video_demuxer.h"
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
#include "hud_draw.h"
#include "max2_eac_view.h"
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

// The overlay RT matches the encode dimensions 1:1 (u_overlay_rect
// spans the whole picture), so HUD elements land pixel-exact on the
// output. Layout matches the playback overlays: speed gauge bottom-
// left, minimap bottom-right, both scaled by encode_height / 720 so
// the design keeps playback proportions at any output resolution.
constexpr float kHudDesignHeight = 720.0f;
constexpr float kHudPaddingPx    = 16.0f;   // at design scale
constexpr float kGaugeRadiusPx   = 90.0f;   // at design scale
constexpr float kMapSizePx       = 280.0f;  // at design scale

// Number of encode-input slots in the ping-pong. mhevcenc enforces
// at most one outstanding SubmitPicture, but the input texture can
// safely be overwritten as soon as that encode CL has read it. We
// fill slot N while encode CL N-1 is still consuming slot N-1.
constexpr uint32_t kEncodeInputCount = 2;

// Base (1x scale) output dims for reframed .360 sources. The source
// EAC storage dims (4096x1344 per track) are meaningless as an
// output size; a 16:9-class flat window is what a reframe means.
constexpr uint32_t kReframeBaseWidth  = 1920;
constexpr uint32_t kReframeBaseHeight = 1080;

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

} // namespace

struct TranscodeSession::Impl final {
  // ---- Construction-time inputs ---------------------------
  mnexus::IDevice* device = nullptr;
  TranscodeDesc    desc;
  mnexus::QueueId  gfx_queue;
  mnexus::QueueId  encode_queue;

  // ---- Pipeline components --------------------------------
  std::unique_ptr<mplay::MediaPlayer>              player;
  // .360 reframe: second HEVC track's player + the EAC renderer +
  // the flat view target it renders into. All null for flat sources.
  std::unique_ptr<mplay::MediaPlayer>              player_1;
  std::unique_ptr<Max2EacView>                     eac_view;
  bool                                             is_360 = false;
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
  std::unique_ptr<OsmMinimap> osm_minimap_local;  // street-level detail mosaic
  mnexus::TextureHandle       minimap_local_tex;

  // ---- Overlay rendering ----------------------------------
  std::unique_ptr<mshell::ImguiRenderer> overlay_renderer;
  ImDrawList* overlay_drawlist = nullptr;

  // ---- GPU resources --------------------------------------
  std::array<mnexus::TextureHandle, kEncodeInputCount> encode_inputs{};
  mnexus::TextureHandle      scratch_y;
  mnexus::TextureHandle      scratch_cbcr;
  mnexus::TextureHandle      overlay_rgba_tex;
  mnexus::TextureHandle      view_rgb_tex;  // .360 only: reframed EAC view
  mnexus::BufferHandle       compose_ubo;
  mnexus::SamplerHandle      linear_sampler;
  // Flat path: NV12 + overlay -> Y / CbCr. 360 path: reframed RGB +
  // overlay -> Y / CbCr. Only the pair matching the source is
  // compiled.
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
    osm_minimap_local.reset();
    route.clear();
    eac_view.reset();  // frees its own device resources; device must still be alive
    player_1.reset();
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
      if (view_rgb_tex.IsValid())         device->DestroyTexture(view_rgb_tex);
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

    // ----- .360 detection (dual HEVC tracks) ----------------
    if (mdemux::CountHevcVideoTracks(desc.input_path) >= 2) {
      player_1 = mplay::MediaPlayer::Open(device, mplay::OpenMp4Desc{
        .path                 = desc.input_path,
        .video_track_index    = 1,
        .enable_audio         = false,
        .enable_decode_timing = false,
      });
      if (player_1 == nullptr) {
        MBASE_LOG_ERROR("TranscodeSession: failed to open track 1 of dual-track source");
        return false;
      }
      eac_view = Max2EacView::Create(device);
      if (eac_view == nullptr) {
        MBASE_LOG_ERROR("TranscodeSession: Max2EacView::Create failed");
        return false;
      }
      eac_view->SetView(desc.view_yaw_deg, desc.view_pitch_deg, desc.view_fov_deg);
      is_360 = true;
      player_1->SetAutoPlay(false);
      MBASE_LOG_INFO("TranscodeSession: .360 reframe -- yaw {:.1f} pitch {:.1f} fov {:.1f}",
        desc.view_yaw_deg, desc.view_pitch_deg, desc.view_fov_deg);
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

    // Flat sources compose from the decoded NV12 planes; .360
    // sources compose from the reframed RGB view.
    char const* const y_asset    = is_360 ? "compose_rgb_to_y.slang"    : "compose_nv12_to_y.slang";
    char const* const y_module   = is_360 ? "compose_rgb_y"             : "compose_y";
    char const* const cc_asset   = is_360 ? "compose_rgb_to_cbcr.slang" : "compose_nv12_to_cbcr.slang";
    char const* const cc_module  = is_360 ? "compose_rgb_cbcr"          : "compose_cbcr";
    compose_y_vs = CompileSlangModule(device, y_asset, y_module, "vertex_main");
    compose_y_fs = CompileSlangModule(device, y_asset, y_module, "fragment_main");
    {
      std::array<mnexus::ShaderModuleHandle, 2> mods{ compose_y_vs, compose_y_fs };
      compose_y_program = device->CreateProgram(mnexus::ProgramDesc{ .shader_modules = mods });
      if (!compose_y_program.IsValid()) return false;
    }
    compose_cbcr_vs = CompileSlangModule(device, cc_asset, cc_module, "vertex_main");
    compose_cbcr_fs = CompileSlangModule(device, cc_asset, cc_module, "fragment_main");
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
    uint32_t const scale  = desc.encode_scale != 0 ? desc.encode_scale : 1u;
    uint32_t const base_w = is_360 ? kReframeBaseWidth  : eff_width;
    uint32_t const base_h = is_360 ? kReframeBaseHeight : eff_height;
    encode_width  = align_down(base_w / scale, 16);
    encode_height = align_down(base_h / scale, 16);
    if (encode_width == 0u || encode_height == 0u) {
      MBASE_LOG_ERROR("TranscodeSession: encode dims collapse to zero at scale 1/{}", scale);
      return false;
    }
    MBASE_LOG_INFO("TranscodeSession: {} frames @ {}x{} (scale 1/{}) -> {}",
      total_frames, encode_width, encode_height, scale, desc.output_path);

    // Overlay RT matches the encode dims 1:1 so HUD pixels land
    // exactly on output pixels (no compose-time rescale blur).
    overlay_rgba_tex = device->CreateTexture(mnexus::TextureDesc{
      .usage             = mnexus::TextureUsageFlagBits::kAttachment
                         | mnexus::TextureUsageFlagBits::kSampled,
      .format            = mnexus::Format::kR8G8B8A8_UNORM,
      .dimension         = mnexus::TextureDimension::k2D,
      .width             = encode_width,
      .height            = encode_height,
      .depth             = 1,
      .mip_level_count   = 1,
      .array_layer_count = 1,
    });
    if (!overlay_rgba_tex.IsValid()) return false;

    // .360: offscreen target the EAC shader reframes into, sampled
    // by the compose_rgb_to_* shaders. View + dims are fixed for the
    // whole run, so the EAC UBO is written exactly once here.
    if (is_360) {
      view_rgb_tex = device->CreateTexture(mnexus::TextureDesc{
        .usage             = mnexus::TextureUsageFlagBits::kAttachment
                           | mnexus::TextureUsageFlagBits::kSampled,
        .format            = mnexus::Format::kR8G8B8A8_UNORM,
        .dimension         = mnexus::TextureDimension::k2D,
        .width             = encode_width,
        .height            = encode_height,
        .depth             = 1,
        .mip_level_count   = 1,
        .array_layer_count = 1,
      });
      if (!view_rgb_tex.IsValid()) return false;
      eac_view->WriteTargetUbo(encode_width, encode_height);
    }

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

    // One-time UBO upload: rotation matrix + overlay rect. The
    // reframed .360 view is already upright (the rotation rows are
    // unused by the compose_rgb_to_* shaders anyway).
    {
      uint32_t const rot = is_360 ? 0u : source_rotation_degrees;
      float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
      switch (rot) {
        case   0: a =  1.0f; b =  0.0f; c =  0.0f; d =  1.0f; break;
        case  90: a =  0.0f; b = -1.0f; c =  1.0f; d =  0.0f; break;
        case 180: a = -1.0f; b =  0.0f; c =  0.0f; d = -1.0f; break;
        case 270: a =  0.0f; b =  1.0f; c = -1.0f; d =  0.0f; break;
        default:  break;
      }
      // Overlay rect spans the whole picture: the RT is allocated at
      // the encode dims, so overlay UV == picture UV, 1:1 pixels.
      float const ubo_data[12] = {
        a,    b,    0.0f, 0.0f,  // u_uv_rotation_row0
        c,    d,    0.0f, 0.0f,  // u_uv_rotation_row1
        0.0f, 0.0f, 1.0f, 1.0f,  // u_overlay_rect
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

    // Local-detail mosaic -- higher zoom capped at 16x16 tiles. The
    // burned-in map draws a 1:1-pixel viewport of this mosaic that
    // follows the current position, matching the playback minimap.
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

    float const out_w = static_cast<float>(encode_width);
    float const out_h = static_cast<float>(encode_height);
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(out_w, out_h), false);
    dl->PushTextureID(ImGui::GetIO().Fonts->TexID);

    // Telemetry for this frame. No fix (pre-lock lead-in / no GPMF)
    // -> no HUD at all, matching the playback overlays.
    std::optional<mgpmf::Gps9> gps;
    if (cached_gpmf != nullptr) gps = cached_gpmf->gps9();
    if (gps.has_value()) {
      float const s   = out_h / kHudDesignHeight;
      float const pad = kHudPaddingPx * s;

      ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
      float const info_h = font->FontSize * s;

      // ----- Speed gauge (bottom-left) -------------------
      float const r = kGaugeRadiusPx * s;
      ImVec2 const gauge_center(pad + r, out_h - pad - info_h * 1.6f - r);
      hud::DrawSpeedGauge(dl, gauge_center, r, gps->speed_2d * 3.6f);

      // alt / fix / dop line centred under the gauge, on a subtle
      // dark chip so it reads over any footage.
      char info[64];
      std::snprintf(info, sizeof(info), "alt %5.0f m   %s  dop %.1f",
                    gps->altitude, gps->fix >= 3 ? "3D" : "2D", gps->dop);
      ImVec2 const info_sz = font->CalcTextSizeA(info_h, FLT_MAX, 0.0f, info);
      ImVec2 const info_pos(gauge_center.x - info_sz.x * 0.5f,
                            out_h - pad - info_h * 1.3f);
      dl->AddRectFilled(
        ImVec2(info_pos.x - 4.0f * s, info_pos.y - 2.0f * s),
        ImVec2(info_pos.x + info_sz.x + 4.0f * s, info_pos.y + info_sz.y + 2.0f * s),
        IM_COL32(0, 0, 0, 120), 4.0f * s);
      dl->AddText(font, info_h, info_pos, IM_COL32(200, 200, 200, 255), info);

      // ----- Minimap (bottom-right) ----------------------
      // Same design as the playback minimap: a square window of map
      // pixels centred on the current position, casing polyline,
      // marker, attribution. The mosaic is sampled 1:1 (viewport
      // px == mosaic px) so tiles stay as crisp as in playback.
      if (osm_minimap_local != nullptr && minimap_local_tex.IsValid() && !route.empty()) {
        float const map_size = kMapSizePx * s;
        ImVec2 const map_min(out_w - pad - map_size, out_h - pad - map_size);
        ImVec2 const map_max(out_w - pad, out_h - pad);

        size_t traversed = 0;
        for (auto const& e : route) {
          if (e.gpmf_sample_no > cached_sample_no) break;
          ++traversed;
        }
        size_t const cur_idx = traversed > 0 ? traversed - 1 : 0;

        float const base_w = static_cast<float>(osm_minimap_local->base_map_width());
        float const base_h = static_cast<float>(osm_minimap_local->base_map_height());
        float cx = 0.0f;
        float cy = 0.0f;
        osm_minimap_local->ProjectGpsToPixel(route[cur_idx].lat, route[cur_idx].lng, cx, cy);

        float const vp_x0 = std::clamp(cx - map_size * 0.5f,
                                       0.0f, std::max(0.0f, base_w - map_size));
        float const vp_y0 = std::clamp(cy - map_size * 0.5f,
                                       0.0f, std::max(0.0f, base_h - map_size));
        float const u0 = vp_x0 / base_w;
        float const v0 = vp_y0 / base_h;
        float const u1 = std::min((vp_x0 + map_size) / base_w, 1.0f);
        float const v1 = std::min((vp_y0 + map_size) / base_h, 1.0f);

        dl->AddImage(static_cast<ImTextureID>(minimap_local_tex.Get()),
                     map_min, map_max, ImVec2(u0, v0), ImVec2(u1, v1));

        // Route polyline in mosaic pixel space, decimated for very
        // long captures, offset into the viewport 1:1.
        dl->PushClipRect(map_min, map_max, true);
        {
          constexpr size_t kMaxPolylinePoints = 1500;
          size_t const stride = std::max<size_t>(1, route.size() / kMaxPolylinePoints);
          std::vector<ImVec2> pts;
          pts.reserve(route.size() / stride + 2);
          int traversed_pts = 0;
          for (size_t i = 0; i < route.size(); i += stride) {
            float mx = 0.0f;
            float my = 0.0f;
            osm_minimap_local->ProjectGpsToPixel(route[i].lat, route[i].lng, mx, my);
            pts.push_back(ImVec2(map_min.x + (mx - vp_x0),
                                 map_min.y + (my - vp_y0)));
            if (i <= cur_idx) traversed_pts = static_cast<int>(pts.size());
          }
          hud::DrawTrackPolyline(dl, pts.data(), static_cast<int>(pts.size()),
                                 traversed_pts, s);
        }
        hud::DrawPositionMarker(
          dl, ImVec2(map_min.x + (cx - vp_x0), map_min.y + (cy - vp_y0)), s);
        hud::DrawOsmAttribution(dl, map_min, map_max, s);
        dl->PopClipRect();
        hud::DrawMapFrame(dl, map_min, map_max);
      }
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

    // ---- 1. mplay decode (both tracks for .360) ---------
    if (frame_n > 0) {
      player->SeekToDisplayIndex(frame_n);
      if (is_360) player_1->SeekToDisplayIndex(frame_n);
    }
    player->Update();
    player->Render();
    if (is_360) {
      player_1->Update();
      player_1->Render();
    }
    take_stage_ms(timing.decode_ms_sum, timing.decode_ms_n);

    // ---- 2. Overlay ImDrawList for this frame's PTS ------
    RefreshGpmfForPts(player->current_pts_seconds());
    BuildOverlayDrawList();

    ImDrawData dd;
    dd.Clear();
    dd.DisplayPos       = ImVec2(0.0f, 0.0f);
    dd.DisplaySize      = ImVec2(static_cast<float>(encode_width),
                                 static_cast<float>(encode_height));
    dd.FramebufferScale = ImVec2(1.0f, 1.0f);
    dd.AddDrawList(overlay_drawlist);
    dd.Valid = true;

    overlay_renderer->UpdateGeometryBuffers(device, &dd);

    // Compose-pass binding-0 sources: the decoded NV12 planes for a
    // flat run, the reframed RGB view for a .360 run (bound to both
    // the Y and CbCr passes; the render below fills it).
    mnexus::TextureHandle const y_src =
      is_360 ? view_rgb_tex : player->current_y_texture();
    mnexus::TextureHandle const cbcr_src =
      is_360 ? view_rgb_tex : player->current_cbcr_texture();

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

    // 3c'. .360: reframe both EAC tracks into the flat view target,
    // then make it readable by the compose passes.
    if (is_360) {
      eac_view->DrawIntoTarget(gfx_cl, view_rgb_tex,
        player->current_y_texture(),   player->current_cbcr_texture(),
        player_1->current_y_texture(), player_1->current_cbcr_texture());
      gfx_cl->TextureBarrier(view_rgb_tex, ColorRange(),
        mnexus::ResourceBarrierStageFlagBits::kFragmentShader,
        mnexus::ResourceBarrierState::kReadOnly);
    }

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
        y_src, ColorRange());
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
        cbcr_src, ColorRange());
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
