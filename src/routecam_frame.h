#pragma once

// c++ headers ------------------------------------------
#include <memory>
#include <string>

// public project headers -------------------------------
#include "mshell/public/mshell.h"

namespace routecam {

/// The RouteCam `IFrame`. Owns the `mplay::MediaPlayer`, the
/// NV12 -> RGB swapchain blit, the File > Open dialog, and the
/// drag-and-drop handler. Hosted by `mshell::IShell`.
class RouteCamFrame final : public mshell::IFrame {
public:
  explicit RouteCamFrame(std::string initial_path);
  ~RouteCamFrame() override;

  void OnAttach(mshell::AttachContext const& ctx) override;
  void OnDetach() override;
  void OnEvent(mshell::PlatformEvent const& event) override;
  void OnNewFrame(mshell::NewFrameContext const& ctx) override;
  void OnRender(mshell::RenderContext const& ctx) override;

  /// Swaps the playing file. Called by the File > Open dialog and
  /// by the `SDL_DROPFILE` handler. Returns false if `mp4_path`
  /// could not be opened, in which case the player is left null.
  bool LoadFile(std::string const& mp4_path);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routecam
