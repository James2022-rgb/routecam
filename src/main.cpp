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
      std::this_thread::yield();
    } else {
      shell_raw->OnNewFrame();
      std::this_thread::yield();
      shell_raw->OnTick();
    }
  }

  SDL_DestroyWindow(window);
  SDL_Quit();

  shell_raw->OnFinalize();
  delete shell_raw;

  mbase::Logger::Shutdown();
  return 0;
}
