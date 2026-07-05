// platform detection headers ---------------------------
#include "mbase/public/platform.h"

// c++ headers ------------------------------------------
#include <clocale>
#include <string>
#include <thread>

// external headers -------------------------------------
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#if MBASE_PLATFORM_WINDOWS
# include <objbase.h>
# include <shobjidl.h>
#endif

// public project headers -------------------------------
#include "mbase/public/log.h"
#include "mshell/public/mshell.h"

// project headers --------------------------------------
#include "routecam_frame.h"

namespace {

std::string FirstFileArg(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    char const* a = argv[i];
    if (a == nullptr || a[0] == '\0' || a[0] == '-') continue;
    return std::string{a};
  }
  return std::string{};
}

#if MBASE_PLATFORM_WINDOWS

/// Windows-taskbar progress bar (the green fill over the taskbar
/// button) driven by `RouteCamFrame::background_progress`. COM /
/// ITaskbarList3; failures at any step just disable the feature.
class TaskbarProgress final {
public:
  TaskbarProgress(HWND hwnd) : hwnd_(hwnd) {
    com_initialized_ = SUCCEEDED(
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    if (!com_initialized_) return;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&taskbar_)))) {
      taskbar_ = nullptr;
      return;
    }
    if (FAILED(taskbar_->HrInit())) {
      taskbar_->Release();
      taskbar_ = nullptr;
    }
  }

  ~TaskbarProgress() {
    if (taskbar_ != nullptr) {
      taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS);
      taskbar_->Release();
    }
    if (com_initialized_) CoUninitialize();
  }

  void Update(std::optional<routecam::RouteCamFrame::BackgroundProgress> const& progress) {
    if (taskbar_ == nullptr) return;

    TBPFLAG const state = !progress.has_value() ? TBPF_NOPROGRESS
                        : progress->error       ? TBPF_ERROR
                                                : TBPF_NORMAL;
    ULONGLONG const permille = progress.has_value()
      ? static_cast<ULONGLONG>(progress->fraction * 1000.0f + 0.5f)
      : 0ull;

    // Only touch the API when something actually changed; this runs
    // every frame of the main loop.
    if (state != last_state_) {
      taskbar_->SetProgressState(hwnd_, state);
      last_state_    = state;
      last_permille_ = ~0ull;
    }
    if (state != TBPF_NOPROGRESS && permille != last_permille_) {
      taskbar_->SetProgressValue(hwnd_, permille, 1000ull);
      last_permille_ = permille;
    }
  }

private:
  HWND           hwnd_            = nullptr;
  ITaskbarList3* taskbar_         = nullptr;
  bool           com_initialized_ = false;
  TBPFLAG        last_state_      = TBPF_NOPROGRESS;
  ULONGLONG      last_permille_   = ~0ull;
};

#endif // MBASE_PLATFORM_WINDOWS

} // namespace

int main(int argc, char** argv) {
  constexpr unsigned int kWidth  = 1280;
  constexpr unsigned int kHeight = 720;

  // C runtime locale = UTF-8 so MSVC's std::filesystem string()
  // conversions (used by ImGuiFileDialog) don't throw on non-ASCII
  // paths. Windows 10 1903+; no-op elsewhere.
  std::setlocale(LC_ALL, ".UTF-8");

  mbase::Logger::Initialize();

  std::string const file_arg = FirstFileArg(argc, argv);
  MBASE_LOG_INFO("routecam: file_arg=\"{}\"", file_arg);

  mshell::ShellCreateDesc desc{};
  desc.frame = std::make_unique<routecam::RouteCamFrame>(file_arg);
  // The shell owns the frame; this raw pointer stays valid until
  // OnFinalize, which the main loop never outlives.
  routecam::RouteCamFrame* const frame_raw =
    static_cast<routecam::RouteCamFrame*>(desc.frame.get());

  // Leaked on purpose: matches the Emscripten-friendly main-returns-but-
  // runtime-stays-alive pattern. On desktop this is irrelevant; the OS
  // reaps the process on exit.
  mshell::IShell* shell_raw = mshell::IShell::Create(std::move(desc)).release();

  SDL_Init(0);

  SDL_Window* const window = SDL_CreateWindow(
    "RouteCam",
    SDL_WINDOWPOS_CENTERED,
    SDL_WINDOWPOS_CENTERED,
    static_cast<int>(kWidth), static_cast<int>(kHeight),
    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
  );

  auto MakeSurfaceInfo = [](SDL_Window* const window) -> mshell::PlatformSurfaceInfo {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

    return mshell::PlatformSurfaceInfo{
#if MBASE_PLATFORM_WINDOWS
      .instance_handle = reinterpret_cast<uint64_t>(wmInfo.info.win.hinstance),
      .display_handle  = 0,
      .window_handle   = reinterpret_cast<uint64_t>(wmInfo.info.win.window),
#elif MBASE_PLATFORM_LINUX
      .instance_handle = 0,
      .display_handle  = reinterpret_cast<uint64_t>(wmInfo.info.x11.display),
      .window_handle   = reinterpret_cast<uint64_t>(wmInfo.info.x11.window),
#endif
      .imgui_sdl_window = reinterpret_cast<uint64_t>(window),
    };
  };

  shell_raw->OnSurfaceRecreated(MakeSurfaceInfo(window));

#if MBASE_PLATFORM_WINDOWS
  TaskbarProgress taskbar_progress(
    reinterpret_cast<HWND>(MakeSurfaceInfo(window).window_handle));
#endif

  bool paused = false;
  bool quit   = false;

  SDL_Event evt;
  while (!quit) {
    while (SDL_PollEvent(&evt)) {
      // Forward to the shell first (ImGui SDL backend + the frame's
      // OnEvent handler get to look at it).
      shell_raw->ProcessPlatformEvent(mshell::PlatformEvent{ .sdl_event = &evt });

      switch (evt.type) {
      case SDL_QUIT:
        quit = true;
        break;
      case SDL_WINDOWEVENT:
        if (evt.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
          shell_raw->OnSurfaceDestroyed();
          shell_raw->OnSurfaceRecreated(MakeSurfaceInfo(window));
        } else if (evt.window.event == SDL_WINDOWEVENT_MINIMIZED) {
          paused = true;
        } else if (evt.window.event == SDL_WINDOWEVENT_RESTORED) {
          shell_raw->OnSurfaceDestroyed();
          shell_raw->OnSurfaceRecreated(MakeSurfaceInfo(window));
          paused = false;
        } else if (evt.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
          shell_raw->OnDisplayChanged();
        }
        break;
      case SDL_DROPFILE:
        // The frame's OnEvent already consumed the path; we free the
        // buffer SDL allocated.
        if (evt.drop.file != nullptr) {
          SDL_free(evt.drop.file);
        }
        break;
      default:
        break;
      }
    }

    if (paused) {
      // Minimized: no rendering, but long-running work (transcode)
      // keeps advancing so the taskbar progress keeps filling.
      if (!frame_raw->TickBackgroundWork()) {
        SDL_Delay(10);  // idle -- don't spin a core
      }
    } else {
      shell_raw->OnNewFrame();
      std::this_thread::yield();
      shell_raw->OnTick();
    }

#if MBASE_PLATFORM_WINDOWS
    // Mirror long-running work (transcode) onto the taskbar button.
    taskbar_progress.Update(frame_raw->background_progress());
#endif
  }

  SDL_DestroyWindow(window);
  SDL_Quit();

  shell_raw->OnFinalize();
  delete shell_raw;

  mbase::Logger::Shutdown();
  return 0;
}
