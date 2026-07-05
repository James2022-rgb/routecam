#pragma once

// c++ system headers -----------------------------------
#include <memory>

// public project headers -------------------------------
#include "mnexus/public/types.h"

// public project headers --------------------------------
#include "mshell/public/mshell.h"

namespace mnexus { class IDevice; }

namespace routecam {

/// Mouse-look renderer for GoPro Max 2 `.360` files (dual-track EAC,
/// 4096x1344 per track, faces packed [side|center|side]). Owns the
/// `max2_eac_view.slang` program, the view UBO, and the view state
/// (yaw / pitch / FOV). The caller owns the two per-track players
/// and passes their display textures to `Render`.
class Max2EacView final {
public:
  /// Compiles the EAC shader and allocates the pass resources.
  /// Returns `nullptr` on shader compile / allocation failure.
  static std::unique_ptr<Max2EacView> Create(mnexus::IDevice* device);

  ~Max2EacView();
  Max2EacView(Max2EacView const&) = delete;
  Max2EacView& operator=(Max2EacView const&) = delete;

  /// Mouse-look input: left-drag outside ImGui windows rotates the
  /// view, wheel zooms FOV. Call once per frame between
  /// `ImGui::NewFrame` and `ImGui::Render`.
  void HandleMouseInput();

  /// Yaw / pitch readouts + FOV / sensitivity sliders + recenter
  /// button. Call inside an open ImGui window (the RouteCam panel).
  void DrawPanelControls();

  /// Renders the full-screen mouse-look pass onto the swapchain
  /// (includes the barrier / clear, mirroring the flat blit path).
  /// `t0_*` are track 0's (LEFT/FRONT/RIGHT) display textures,
  /// `t1_*` track 1's (DOWN/BACK/TOP).
  void Render(mshell::RenderContext const& ctx,
              mnexus::TextureHandle t0_y, mnexus::TextureHandle t0_cbcr,
              mnexus::TextureHandle t1_y, mnexus::TextureHandle t1_cbcr);

  float yaw_degrees() const;
  float pitch_degrees() const;
  float fov_degrees() const;

private:
  Max2EacView() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routecam
