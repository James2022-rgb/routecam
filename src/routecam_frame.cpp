// TU header --------------------------------------------
#include "routecam_frame.h"

// c++ system headers -----------------------------------
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

// external headers --------------------------------------
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "imgui.h"
#include "ImGuiFileDialog.h"

// public project headers --------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"
#include "masset/public/masset.h"
#include "mnexus/public/mnexus.h"
#include "mnexus/public/types.h"
#include "mplay/public/media_player.h"
#include "mslang_proxy/public/mslang_proxy.h"

namespace routecam {

namespace {

// Fallback when launched with no command-line argument. Picked to
// match what the playground prototype used so cold-start runs the
// developer's normal test capture.
constexpr char const* kFallbackPath = R"(J:\Pics\旅行\GX010005.MP4)";

} // namespace

struct RouteCamFrame::Impl final {
  std::string mp4_path;
  mnexus::IDevice* device = nullptr;
  mnexus::INexus*  nexus  = nullptr;
  bool             request_hdr_swapchain = false;

  std::unique_ptr<mplay::MediaPlayer> player;

  mnexus::ShaderModuleHandle vs_handle      = {};
  mnexus::ShaderModuleHandle fs_handle      = {};
  mnexus::ProgramHandle      program_handle = {};
  mnexus::SamplerHandle      sampler_handle = {};
  mnexus::BufferHandle       ubo_handle     = {};
};

RouteCamFrame::RouteCamFrame(std::string initial_path)
  : impl_(std::make_unique<Impl>()) {
  impl_->mp4_path = initial_path.empty() ? kFallbackPath : std::move(initial_path);
}

RouteCamFrame::~RouteCamFrame() = default;

void RouteCamFrame::OnAttach(mshell::AttachContext const& ctx) {
  MBASE_LOG_INFO("RouteCamFrame::OnAttach");
  impl_->device = ctx.device;
  impl_->nexus  = ctx.nexus;

  // ----- Compile NV12 -> RGB blit shader -----
  {
    masset::IAssetManager* asset_manager = masset::IAssetManager::Get();
    MBASE_ASSERT(asset_manager != nullptr);

    char const* const shader_asset_path = "nv12_to_rgb.slang";
    char const* const module_name       = "nv12_to_rgb";
    char const* const capability_name   = "GLSL_150";

    std::vector<std::byte> shader_bytes;
    uint64_t shader_timestamp = 0;
    MBASE_ASSERT(asset_manager->LoadAssetEx(shader_asset_path, shader_bytes, shader_timestamp));
    std::string slang_code(reinterpret_cast<char const*>(shader_bytes.data()), shader_bytes.size());

    std::optional<std::vector<uint32_t>> opt_vs = mslang_proxy::CompileSlangToSpirv(
      module_name, shader_asset_path, slang_code.c_str(), nullptr, capability_name, "vertex_main");
    MBASE_ASSERT(opt_vs.has_value());
    std::optional<std::vector<uint32_t>> opt_fs = mslang_proxy::CompileSlangToSpirv(
      module_name, shader_asset_path, slang_code.c_str(), nullptr, capability_name, "fragment_main");
    MBASE_ASSERT(opt_fs.has_value());

    impl_->vs_handle = impl_->device->CreateShaderModule(mnexus::ShaderModuleDesc{
      .source_language    = mnexus::ShaderSourceLanguage::kSpirV,
      .code_ptr           = reinterpret_cast<uint64_t>(opt_vs->data()),
      .code_size_in_bytes = static_cast<uint32_t>(opt_vs->size() * sizeof(uint32_t)),
    });
    impl_->fs_handle = impl_->device->CreateShaderModule(mnexus::ShaderModuleDesc{
      .source_language    = mnexus::ShaderSourceLanguage::kSpirV,
      .code_ptr           = reinterpret_cast<uint64_t>(opt_fs->data()),
      .code_size_in_bytes = static_cast<uint32_t>(opt_fs->size() * sizeof(uint32_t)),
    });
    std::array<mnexus::ShaderModuleHandle, 2> mods{ impl_->vs_handle, impl_->fs_handle };
    impl_->program_handle = impl_->device->CreateProgram(mnexus::ProgramDesc{
      .shader_modules = mods,
    });
  }

  impl_->sampler_handle = impl_->device->CreateSampler(mnexus::SamplerDesc{
    .min_filter     = mnexus::Filter::kLinear,
    .mag_filter     = mnexus::Filter::kLinear,
    .mipmap_filter  = mnexus::Filter::kNearest,
    .address_mode_u = mnexus::AddressMode::kClampToEdge,
    .address_mode_v = mnexus::AddressMode::kClampToEdge,
    .address_mode_w = mnexus::AddressMode::kClampToEdge,
  });
  impl_->ubo_handle = impl_->device->CreateBuffer(mnexus::BufferDesc{
    .usage         = mnexus::BufferUsageFlagBits::kUniform,
    .size_in_bytes = 80,
  });

  if (!LoadFile(impl_->mp4_path)) {
    MBASE_LOG_WARN("RouteCamFrame::OnAttach: initial LoadFile({}) failed", impl_->mp4_path);
  }
}

void RouteCamFrame::OnDetach() {
  MBASE_LOG_INFO("RouteCamFrame::OnDetach");
  if (impl_->device != nullptr) {
    if (impl_->ubo_handle.IsValid())     impl_->device->DestroyBuffer(impl_->ubo_handle);
    if (impl_->sampler_handle.IsValid()) impl_->device->DestroySampler(impl_->sampler_handle);
    if (impl_->program_handle.IsValid()) impl_->device->DestroyProgram(impl_->program_handle);
    if (impl_->fs_handle.IsValid())      impl_->device->DestroyShaderModule(impl_->fs_handle);
    if (impl_->vs_handle.IsValid())      impl_->device->DestroyShaderModule(impl_->vs_handle);
  }
  impl_->player.reset();
}

void RouteCamFrame::OnEvent(mshell::PlatformEvent const& event) {
#if MBASE_PLATFORM_WINDOWS || MBASE_PLATFORM_LINUX
  if (event.sdl_event == nullptr) return;
  if (event.sdl_event->type == SDL_DROPFILE && event.sdl_event->drop.file != nullptr) {
    // The shell's main loop owns the SDL_free of `drop.file`; we just
    // copy the bytes we need.
    LoadFile(std::string{event.sdl_event->drop.file});
  }
#else
  (void)event;
#endif
}

bool RouteCamFrame::LoadFile(std::string const& mp4_path) {
  std::string const path = mp4_path.empty() ? std::string{kFallbackPath} : mp4_path;
  MBASE_LOG_INFO("RouteCamFrame::LoadFile: opening {}", path);

  // Destroy the existing player BEFORE opening the new one (NVDEC
  // session limits + audio sink double-binding both crash if two
  // players try to coexist across an Open call).
  impl_->player.reset();

  auto new_player = mplay::MediaPlayer::Open(impl_->device, mplay::OpenMp4Desc{
    .path                 = path,
    .video_track_index    = 0,
    .enable_audio         = true,
    .enable_decode_timing = true,
  });
  if (new_player == nullptr) {
    MBASE_LOG_WARN("RouteCamFrame::LoadFile: open failed for {}", path);
    return false;  // impl_->player is null; OnRender / OnNewFrame handle that.
  }

  impl_->player   = std::move(new_player);
  impl_->mp4_path = path;

  auto const& info = impl_->player->info();
  MBASE_LOG_INFO("RouteCamFrame::LoadFile: {} -- {}x{} {}-bit hdr10={} peak_nits={} frames={} dur={:.2f}s audio={}",
    path, info.width, info.height, info.bit_depth,
    info.is_hdr10 ? "yes" : "no", info.hdr_peak_nits,
    info.total_display_frames, info.total_seconds,
    info.has_audio ? "yes" : "no");

  if (impl_->nexus != nullptr) {
    auto const surf_cap = impl_->nexus->GetSurfaceCapability();
    bool const want_hdr =
      info.is_hdr10 && (surf_cap.GetHdr10ColorFormat() != nullptr);
    if (want_hdr != impl_->request_hdr_swapchain) {
      impl_->nexus->RequestSwapchainRecreation(mnexus::SwapchainRecreateDesc{
        .flags = want_hdr
                   ? mnexus::SwapchainCreateFlags(mnexus::SwapchainCreateFlagBits::kHdr)
                   : mnexus::SwapchainCreateFlags(mnexus::SwapchainCreateFlagBits::kNone),
      });
      impl_->request_hdr_swapchain = want_hdr;
    }
  }
  return true;
}

void RouteCamFrame::OnNewFrame(mshell::NewFrameContext const& /*ctx*/) {
  // ----- Main menu bar (File > Open) + Ctrl+O hotkey ----
  bool want_open_dialog = false;
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Open...", "Ctrl+O")) {
        want_open_dialog = true;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
    want_open_dialog = true;
  }
  if (want_open_dialog) {
    IGFD::FileDialogConfig cfg;
    cfg.path = ".";
    ImGuiFileDialog::Instance()->OpenDialog(
      "RouteCamOpenDlg",
      "Open MP4",
      "Action camera video{.MP4,.mp4,.mov,.MOV,.360}",
      cfg);
  }
  if (ImGuiFileDialog::Instance()->Display("RouteCamOpenDlg")) {
    if (ImGuiFileDialog::Instance()->IsOk()) {
      std::string const picked = ImGuiFileDialog::Instance()->GetFilePathName();
      if (!LoadFile(picked)) {
        MBASE_LOG_WARN("RouteCamFrame: dialog-picked LoadFile failed: {}", picked);
      }
    }
    ImGuiFileDialog::Instance()->Close();
  }

  if (ImGui::Begin("RouteCam")) {
    ImGui::TextDisabled("%s", impl_->mp4_path.c_str());
    ImGui::Separator();

    if (impl_->player == nullptr) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "No file open. Use File > Open... or drop a file here.");
    } else {
      auto const& info = impl_->player->info();
      uint32_t const total = info.total_display_frames;
      uint32_t const cur   = impl_->player->current_display_index();

      ImGui::Text("Display: %u / %u", cur + 1, total);
      ImGui::Text("PTS: %.3fs   Duration: %.2fs",
                  impl_->player->current_pts_seconds(), info.total_seconds);

      bool auto_play = impl_->player->auto_play();
      if (ImGui::Checkbox("Play", &auto_play)) {
        impl_->player->SetAutoPlay(auto_play);
      }
      ImGui::SameLine();
      ImGui::TextDisabled(info.has_audio ? "(audio-master clock)"
                                         : "(wall-clock pacing)");

      if (info.is_hdr10 && impl_->nexus != nullptr) {
        auto const surf_cap = impl_->nexus->GetSurfaceCapability();
        bool const monitor_hdr10 = (surf_cap.GetHdr10ColorFormat() != nullptr);
        ImGui::BeginDisabled(!monitor_hdr10);
        bool req = impl_->request_hdr_swapchain;
        if (ImGui::Checkbox("HDR10 swapchain", &req)) {
          impl_->nexus->RequestSwapchainRecreation(mnexus::SwapchainRecreateDesc{
            .flags = req ? mnexus::SwapchainCreateFlags(mnexus::SwapchainCreateFlagBits::kHdr)
                         : mnexus::SwapchainCreateFlags(mnexus::SwapchainCreateFlagBits::kNone),
          });
          impl_->request_hdr_swapchain = req;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        auto const cs = impl_->nexus->GetSwapchainSurfaceColorSpace();
        char const* cs_label = (cs == mnexus::ColorSpace::kHdr10St2084) ? "active: HDR10 PQ"
                              : (cs == mnexus::ColorSpace::kSrgb)        ? "active: sRGB"
                                                                         : "active: linear";
        ImGui::TextDisabled("(%s, peak %.0f nits%s)",
                            cs_label,
                            info.hdr_peak_nits,
                            monitor_hdr10 ? "" : ", monitor SDR-only");
      }

      if (total > 0) {
        if (ImGui::Button("<<")) impl_->player->SeekByDeltaSeconds(-10.0);
        ImGui::SameLine();
        if (ImGui::Button("<"))  impl_->player->SeekByDeltaSeconds(-5.0);
        ImGui::SameLine();
        if (ImGui::Button(">"))  impl_->player->SeekByDeltaSeconds(+5.0);
        ImGui::SameLine();
        if (ImGui::Button(">>")) impl_->player->SeekByDeltaSeconds(+10.0);

        int slider_value = static_cast<int>(cur);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##seek", &slider_value, 0, static_cast<int>(total) - 1,
                             "Display %d", ImGuiSliderFlags_AlwaysClamp)) {
          impl_->player->SetAutoPlay(false);
          impl_->player->SeekToDisplayIndex(static_cast<uint32_t>(slider_value));
        }
      }
    }
  }
  ImGui::End();
}

void RouteCamFrame::OnRender(mshell::RenderContext const& ctx) {
  if (impl_->player == nullptr) {
    // Clear so the previous frame doesn't ghost.
    ctx.command_list->TextureBarrier(
      ctx.swapchain_texture,
      mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0),
      mnexus::ResourceBarrierStageFlagBits::kColorAttachmentOutput,
      mnexus::ResourceBarrierState::kAttachment);
    mnexus::ClearValue clear{};
    clear.color.r = 0.05f; clear.color.g = 0.05f; clear.color.b = 0.05f; clear.color.a = 1.0f;
    mnexus::ColorAttachmentDesc const color{
      .texture           = ctx.swapchain_texture,
      .subresource_range = mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0),
      .load_op           = mnexus::LoadOp::kClear,
      .store_op          = mnexus::StoreOp::kStore,
      .clear_value       = clear,
    };
    ctx.command_list->BeginRenderPass(mnexus::RenderPassDesc{
      .color_attachments = color,
    });
    ctx.command_list->EndRenderPass();
    return;
  }

  impl_->player->Update();
  impl_->player->Render();

  // ----- Refresh nv12_to_rgb UBO -----
  auto const& info = impl_->player->info();
  {
    uint32_t const rot = info.rotation_degrees;
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
    switch (rot) {
      case   0: a =  1.0f; b =  0.0f; c =  0.0f; d =  1.0f; break;
      case  90: a =  0.0f; b = -1.0f; c =  1.0f; d =  0.0f; break;
      case 180: a = -1.0f; b =  0.0f; c =  0.0f; d = -1.0f; break;
      case 270: a =  0.0f; b =  1.0f; c = -1.0f; d =  0.0f; break;
      default:  break;
    }

    mnexus::TextureDesc swapchain_desc{};
    ctx.device->GetTextureDesc(ctx.swapchain_texture, swapchain_desc);
    bool const swapchain_is_hdr10 =
      swapchain_desc.format == mnexus::Format::kA2B10G10R10_UNORM_PACK32 ||
      swapchain_desc.format == mnexus::Format::kA2R10G10B10_UNORM_PACK32;
    bool const swapchain_is_srgb =
      swapchain_desc.format == mnexus::Format::kR8G8B8A8_SRGB ||
      swapchain_desc.format == mnexus::Format::kB8G8R8A8_SRGB;
    float const input_kind  = info.is_hdr10 ? 1.0f : 0.0f;
    float const output_kind = swapchain_is_hdr10 ? 2.0f
                            : (swapchain_is_srgb ? 0.0f : 1.0f);

    bool const swap_aspect = (rot == 90 || rot == 270);
    float const eff_w  = static_cast<float>(swap_aspect ? info.height : info.width);
    float const eff_h  = static_cast<float>(swap_aspect ? info.width  : info.height);
    float const sw_w   = static_cast<float>(swapchain_desc.width);
    float const sw_h   = static_cast<float>(swapchain_desc.height);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (eff_w > 0.0f && eff_h > 0.0f && sw_w > 0.0f && sw_h > 0.0f) {
      float const video_aspect  = eff_w / eff_h;
      float const screen_aspect = sw_w / sw_h;
      if (video_aspect > screen_aspect) {
        scale_y = screen_aspect / video_aspect;
      } else {
        scale_x = video_aspect / screen_aspect;
      }
    }

    float const ubo_data[20] = {
      a,                   b,           0.0f, 0.0f,
      c,                   d,           0.0f, 0.0f,
      input_kind,          output_kind, 0.0f, 0.0f,
      scale_x,             scale_y,     0.0f, 0.0f,
      info.hdr_peak_nits,  0.0f,        0.0f, 0.0f,
    };
    impl_->device->QueueWriteBuffer({}, impl_->ubo_handle, 0, ubo_data, sizeof(ubo_data));
  }

  // ----- Swapchain blit -----
  ctx.command_list->TextureBarrier(
    ctx.swapchain_texture,
    mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0),
    mnexus::ResourceBarrierStageFlagBits::kColorAttachmentOutput,
    mnexus::ResourceBarrierState::kAttachment);

  mnexus::ClearValue clear_value{};
  clear_value.color.r = 0.0f; clear_value.color.g = 0.0f;
  clear_value.color.b = 0.0f; clear_value.color.a = 1.0f;
  mnexus::ColorAttachmentDesc const color_attachment{
    .texture           = ctx.swapchain_texture,
    .subresource_range = mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0),
    .load_op           = mnexus::LoadOp::kClear,
    .store_op          = mnexus::StoreOp::kStore,
    .clear_value       = clear_value,
  };
  ctx.command_list->BeginRenderPass(mnexus::RenderPassDesc{
    .color_attachments = color_attachment,
  });

  ctx.command_list->BindRenderProgram(impl_->program_handle);
  auto const display_subres = mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0);
  ctx.command_list->BindSampledTexture(
    mnexus::BindingId{ .group = 0, .binding = 0, .array_element = 0 },
    impl_->player->current_y_texture(), display_subres);
  ctx.command_list->BindSampledTexture(
    mnexus::BindingId{ .group = 0, .binding = 1, .array_element = 0 },
    impl_->player->current_cbcr_texture(), display_subres);
  ctx.command_list->BindSampler(
    mnexus::BindingId{ .group = 0, .binding = 2, .array_element = 0 },
    impl_->sampler_handle);
  ctx.command_list->BindUniformBuffer(
    mnexus::BindingId{ .group = 0, .binding = 3, .array_element = 0 },
    impl_->ubo_handle, 0, 80);

  ctx.command_list->Draw(6, 1, 0, 0);

  ctx.command_list->EndRenderPass();
}

} // namespace routecam
