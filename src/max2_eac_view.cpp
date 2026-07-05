// TU header --------------------------------------------
#include "max2_eac_view.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// external headers --------------------------------------
#include "imgui.h"

// public project headers --------------------------------
#include "masset/public/masset.h"
#include "mbase/public/assert.h"
#include "mbase/public/log.h"
#include "mnexus/public/mnexus.h"
#include "mslang_proxy/public/mslang_proxy.h"

namespace routecam {

namespace {

// Source layout (from ffprobe against Max 2 assets; matches the
// trekview reverse-engineering articles): both video tracks are
// 4096x1344, three faces side-by-side as
//   [ side(1376) | center(1344) | side(1376) ]
// where the corner faces' inner 32 px overlap the center face for
// lens-stitch blending (handled inside the shader).
constexpr float kFrameW       = 4096.0f;
constexpr float kFaceSideW    = 1376.0f;
constexpr float kFaceCenterW  = 1344.0f;
constexpr float kCol0Left =    0.0f / kFrameW;
constexpr float kCol0W    = kFaceSideW   / kFrameW;
constexpr float kCol1Left = kFaceSideW   / kFrameW;
constexpr float kCol1W    = kFaceCenterW / kFrameW;
constexpr float kCol2Left = (kFaceSideW + kFaceCenterW) / kFrameW;
constexpr float kCol2W    = kFaceSideW   / kFrameW;

constexpr double kPi = 3.14159265358979323846;

float DegToRad(float deg) { return deg * static_cast<float>(kPi) / 180.0f; }

} // namespace

struct Max2EacView::Impl final {
  mnexus::IDevice* device = nullptr;

  mnexus::ShaderModuleHandle vs_handle;
  mnexus::ShaderModuleHandle fs_handle;
  mnexus::ProgramHandle      program_handle;
  mnexus::SamplerHandle      sampler_handle;
  mnexus::BufferHandle       ubo_handle;

  bool  clip_space_y_down            = true;
  float yaw_deg                      = 0.0f;
  float pitch_deg                    = 0.0f;
  float fov_deg                      = 90.0f;
  float mouse_sensitivity_deg_per_px = 0.25f;

  ~Impl() {
    if (device != nullptr) {
      if (ubo_handle.IsValid())     device->DestroyBuffer(ubo_handle);
      if (sampler_handle.IsValid()) device->DestroySampler(sampler_handle);
      if (program_handle.IsValid()) device->DestroyProgram(program_handle);
      if (fs_handle.IsValid())      device->DestroyShaderModule(fs_handle);
      if (vs_handle.IsValid())      device->DestroyShaderModule(vs_handle);
    }
  }
};

std::unique_ptr<Max2EacView> Max2EacView::Create(mnexus::IDevice* device) {
  auto view = std::unique_ptr<Max2EacView>(new Max2EacView());
  view->impl_ = std::make_unique<Impl>();
  Impl& impl = *view->impl_;
  impl.device = device;

  // ----- Compile the EAC viewer shader -----
  {
    masset::IAssetManager* asset_manager = masset::IAssetManager::Get();
    MBASE_ASSERT(asset_manager != nullptr);

    char const* const shader_asset_path = "max2_eac_view.slang";
    char const* const module_name       = "max2_eac_view";
    char const* const capability_name   = "GLSL_150";

    std::vector<std::byte> shader_bytes;
    uint64_t shader_timestamp = 0;
    if (!asset_manager->LoadAssetEx(shader_asset_path, shader_bytes, shader_timestamp)) {
      MBASE_LOG_ERROR("Max2EacView: failed to load {}", shader_asset_path);
      return nullptr;
    }
    std::string slang_code(reinterpret_cast<char const*>(shader_bytes.data()),
                           shader_bytes.size());

    std::optional<std::vector<uint32_t>> opt_vs = mslang_proxy::CompileSlangToSpirv(
      module_name, shader_asset_path, slang_code.c_str(), nullptr,
      capability_name, "vertex_main");
    std::optional<std::vector<uint32_t>> opt_fs = mslang_proxy::CompileSlangToSpirv(
      module_name, shader_asset_path, slang_code.c_str(), nullptr,
      capability_name, "fragment_main");
    if (!opt_vs.has_value() || !opt_fs.has_value()) {
      MBASE_LOG_ERROR("Max2EacView: slang compile failed");
      return nullptr;
    }

    impl.vs_handle = device->CreateShaderModule(mnexus::ShaderModuleDesc{
      .source_language    = mnexus::ShaderSourceLanguage::kSpirV,
      .code_ptr           = reinterpret_cast<uint64_t>(opt_vs->data()),
      .code_size_in_bytes = static_cast<uint32_t>(opt_vs->size() * sizeof(uint32_t)),
    });
    impl.fs_handle = device->CreateShaderModule(mnexus::ShaderModuleDesc{
      .source_language    = mnexus::ShaderSourceLanguage::kSpirV,
      .code_ptr           = reinterpret_cast<uint64_t>(opt_fs->data()),
      .code_size_in_bytes = static_cast<uint32_t>(opt_fs->size() * sizeof(uint32_t)),
    });
    if (!impl.vs_handle.IsValid() || !impl.fs_handle.IsValid()) return nullptr;

    std::array<mnexus::ShaderModuleHandle, 2> mods{ impl.vs_handle, impl.fs_handle };
    impl.program_handle = device->CreateProgram(mnexus::ProgramDesc{
      .shader_modules = mods,
    });
    if (!impl.program_handle.IsValid()) return nullptr;
  }

  impl.sampler_handle = device->CreateSampler(mnexus::SamplerDesc{
    .min_filter     = mnexus::Filter::kLinear,
    .mag_filter     = mnexus::Filter::kLinear,
    .mipmap_filter  = mnexus::Filter::kNearest,
    .address_mode_u = mnexus::AddressMode::kClampToEdge,
    .address_mode_v = mnexus::AddressMode::kClampToEdge,
    .address_mode_w = mnexus::AddressMode::kClampToEdge,
  });

  // 6 vec4 = 96 bytes; round up for alignment safety.
  impl.ubo_handle = device->CreateBuffer(mnexus::BufferDesc{
    .usage         = mnexus::BufferUsageFlagBits::kUniform,
    .size_in_bytes = 128,
  });
  if (!impl.sampler_handle.IsValid() || !impl.ubo_handle.IsValid()) return nullptr;

  impl.clip_space_y_down =
    device->GetClipSpaceConvention().y_direction == mnexus::ClipSpaceYDirection::kDown;

  return view;
}

Max2EacView::~Max2EacView() = default;

void Max2EacView::HandleMouseInput() {
  Impl& impl = *impl_;
  ImGuiIO const& io = ImGui::GetIO();
  if (io.WantCaptureMouse) return;

  // Drag rotates the view; consume the running delta so the next
  // frame's GetMouseDragDelta gives incremental motion.
  if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
    ImVec2 const delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    impl.yaw_deg   += delta.x * impl.mouse_sensitivity_deg_per_px;
    impl.pitch_deg -= delta.y * impl.mouse_sensitivity_deg_per_px;
    // Wrap yaw into [-180, 180] for nicer readouts.
    if      (impl.yaw_deg >  180.0f) impl.yaw_deg -= 360.0f;
    else if (impl.yaw_deg < -180.0f) impl.yaw_deg += 360.0f;
    impl.pitch_deg = std::clamp(impl.pitch_deg, -89.0f, 89.0f);
  }
  // Wheel zooms FOV.
  if (io.MouseWheel != 0.0f) {
    impl.fov_deg = std::clamp(impl.fov_deg - io.MouseWheel * 3.0f, 20.0f, 130.0f);
  }
}

void Max2EacView::DrawPanelControls() {
  Impl& impl = *impl_;
  ImGui::Text("360: drag to look, wheel to zoom");
  ImGui::Text("Yaw %+7.2f  Pitch %+7.2f", impl.yaw_deg, impl.pitch_deg);
  ImGui::SliderFloat("FOV (deg)", &impl.fov_deg, 20.0f, 130.0f, "%.1f");
  ImGui::SliderFloat("Mouse deg/px", &impl.mouse_sensitivity_deg_per_px,
                     0.05f, 1.0f, "%.2f");
  if (ImGui::Button("Recenter view")) {
    impl.yaw_deg   = 0.0f;
    impl.pitch_deg = 0.0f;
  }
}

void Max2EacView::Render(mshell::RenderContext const& ctx,
                         mnexus::TextureHandle t0_y, mnexus::TextureHandle t0_cbcr,
                         mnexus::TextureHandle t1_y, mnexus::TextureHandle t1_cbcr) {
  Impl& impl = *impl_;

  // ----- UBO -----
  {
    // View rotation = R_yaw(z-axis) * R_pitch(x-axis) applied to a
    // camera-space ray (initially looking down +y).
    float const yaw_rad   = DegToRad(impl.yaw_deg);
    float const pitch_rad = DegToRad(impl.pitch_deg);
    float const cy = std::cos(yaw_rad),   sy = std::sin(yaw_rad);
    float const cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
    float const r00 = cy;    float const r01 = -sy * cp;  float const r02 =  sy * sp;
    float const r10 = sy;    float const r11 =  cy * cp;  float const r12 = -cy * sp;
    float const r20 = 0.0f;  float const r21 =  sp;       float const r22 =  cp;

    mnexus::TextureDesc swapchain_desc{};
    ctx.device->GetTextureDesc(ctx.swapchain_texture, swapchain_desc);
    float const sw_w = static_cast<float>(swapchain_desc.width);
    float const sw_h = static_cast<float>(swapchain_desc.height);
    float const aspect = (sw_h > 0.0f) ? (sw_w / sw_h) : 1.0f;
    float const fov_t = std::tan(DegToRad(impl.fov_deg) * 0.5f);

    bool const swapchain_is_srgb =
      swapchain_desc.format == mnexus::Format::kR8G8B8A8_SRGB ||
      swapchain_desc.format == mnexus::Format::kB8G8R8A8_SRGB;
    float const output_kind = swapchain_is_srgb ? 0.0f : 1.0f;

    // NDC y sign: the shader treats +ndc.y as "screen up"; Vulkan's
    // default has +ndc.y pointing DOWN after the viewport flip.
    float const ndc_y_sign = impl.clip_space_y_down ? -1.0f : +1.0f;

    float const ubo_data[24] = {
      r00, r01, r02, 0.0f,                     // u_view_rot_r0
      r10, r11, r12, 0.0f,                     // u_view_rot_r1
      r20, r21, r22, 0.0f,                     // u_view_rot_r2
      fov_t, aspect, ndc_y_sign, output_kind,  // u_view_params
      kCol0Left, kCol1Left, kCol2Left, 0.0f,   // u_face_x_left
      kCol0W,    kCol1W,    kCol2W,    0.0f,   // u_face_x_width
    };
    impl.device->QueueWriteBuffer({}, impl.ubo_handle, 0, ubo_data, sizeof(ubo_data));
  }

  // ----- Swapchain pass -----
  ctx.command_list->TextureBarrier(
    ctx.swapchain_texture,
    mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0),
    mnexus::ResourceBarrierStageFlagBits::kColorAttachmentOutput,
    mnexus::ResourceBarrierState::kAttachment);

  mnexus::ClearValue clear{};
  clear.color.r = 0.0f; clear.color.g = 0.0f; clear.color.b = 0.0f; clear.color.a = 1.0f;
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

  ctx.command_list->BindRenderProgram(impl.program_handle);

  auto const subres = mnexus::TextureSubresourceRange::SingleSubresourceColor(0, 0);
  ctx.command_list->BindSampledTexture(
    mnexus::BindingId{ .group = 0, .binding = 0, .array_element = 0 }, t0_y, subres);
  ctx.command_list->BindSampledTexture(
    mnexus::BindingId{ .group = 0, .binding = 1, .array_element = 0 }, t0_cbcr, subres);
  ctx.command_list->BindSampledTexture(
    mnexus::BindingId{ .group = 0, .binding = 2, .array_element = 0 }, t1_y, subres);
  ctx.command_list->BindSampledTexture(
    mnexus::BindingId{ .group = 0, .binding = 3, .array_element = 0 }, t1_cbcr, subres);
  ctx.command_list->BindSampler(
    mnexus::BindingId{ .group = 0, .binding = 4, .array_element = 0 },
    impl.sampler_handle);
  ctx.command_list->BindUniformBuffer(
    mnexus::BindingId{ .group = 0, .binding = 5, .array_element = 0 },
    impl.ubo_handle, 0, 96);

  // Fullscreen triangle.
  ctx.command_list->Draw(3, 1, 0, 0);

  ctx.command_list->EndRenderPass();
}

float Max2EacView::yaw_degrees() const   { return impl_->yaw_deg; }
float Max2EacView::pitch_degrees() const { return impl_->pitch_deg; }
float Max2EacView::fov_degrees() const   { return impl_->fov_deg; }

} // namespace routecam
