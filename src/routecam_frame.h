#pragma once

// c++ headers ------------------------------------------
#include <memory>
#include <optional>
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

  /// Long-running-work readout for the platform layer (e.g. the
  /// Windows taskbar progress bar). `fraction` in [0, 1]; `error`
  /// true when the work aborted (shown until the user dismisses
  /// it in the panel). `nullopt` when nothing long-running is on.
  struct BackgroundProgress final {
    float fraction = 0.0f;
    bool  error    = false;
  };
  std::optional<BackgroundProgress> background_progress() const;

  /// Advances long-running work (the transcode) by one step WITHOUT
  /// rendering. The platform loop calls this while the window is
  /// minimized, where the normal OnNewFrame / OnRender path is
  /// skipped -- a transcode would otherwise stall until restore.
  /// Returns true when work was done (false = idle; the caller may
  /// sleep).
  bool TickBackgroundWork();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routecam
