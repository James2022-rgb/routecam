# RouteCam

Dedicated viewer / transcoder for action-camera footage (GoPro
HERO / Max 2) with GPS + IMU telemetry:

- **Playback**: flat HEVC MP4s and GoPro Max 2 `.360` files
  (dual-track EAC, drag-to-look mouse look), HDR10 swapchain
  output, GPS speed-gauge HUD + OpenStreetMap minimap overlays.
- **Transcode**: burns the HUD into a re-encoded MP4 via Vulkan
  Video (flat sources at source resolution; `.360` sources
  reframed to a fixed flat view chosen in playback), with AAC
  audio passthrough. Windows-taskbar progress; keeps running
  while the window is minimized.

Grew out of the `wentos` playground repo; the history before the
standalone-layout commit refers to paths inside that repo.

## Language

All code comments MUST be written in English.

## Symbols

It is forbidden to use the full width forms of symbols that have
counterparts in ASCII. e.g. `()`, `:`, `,`, `0-9`.

## Repository layout

```
CMakeLists.txt        top-level project -> routecam.exe
src/                  application sources (namespace routecam)
assets/               runtime-loaded .slang shaders
thirdparty/SDL        SDL2 (pinned to the release-2.30.2 tag)
<m-libs>              sibling libraries as submodules (see below)
```

Application sources:

- `main.cpp` -- SDL2 main loop, surface lifecycle, Windows
  taskbar progress (`ITaskbarList3`), minimized-mode background
  ticking.
- `routecam_frame.h/.cpp` -- the `mshell::IFrame`: player
  ownership, File > Open (ImGuiFileDialog) + drag-drop, panel UI,
  playback overlays, transcode launcher.
- `gps_timeline.*` -- GPMF -> media-timeline GPS fixes
  (interpolation, dropout snapping, pre-lock hiding).
- `hud_draw.*` -- shared HUD primitives (speed gauge with
  7-segment readout, track polyline, marker, OSM attribution)
  used identically by playback overlays and the burn-in overlay.
- `minimap.*` / `osm_tile_cache.*` / `slippy_map.h` -- playback
  minimap: async tile cache (WinHTTP fetch -> disk -> GPU).
- `osm_minimap.*` -- synchronous route-mosaic builder used by the
  transcoder.
- `max2_eac_view.*` -- Max 2 EAC mouse-look renderer (swapchain
  pass for playback, offscreen target for the transcode reframe).
- `transcode_session.*` -- decode -> overlay compose -> Vulkan
  Video encode -> multi-track MP4 mux pipeline.

## Submodules

All first-party libraries are `github.com/James2022-rgb/*`
submodules at the repo root: `mbase` (logging / assertions),
`mmath`, `masset` (asset loading; assets resolve as
`<cwd>/assets/...`), `mimage`, `maudio`, `mdemux` / `mmux`
(L-SMASH demux / mux), `maacdec`, `mhevcdec` / `mhevcenc` (Vulkan
Video), `mplay` (media player), `mgpmf` (GPMF telemetry parser,
Rust-backed), `mslang` / `mslang-proxy` (slang shader compiler),
`mnexus` (Vulkan abstraction; `feature/video` branch), `mshell`
(window + ImGui + IFrame lifecycle).

When working on a submodule's code, read that submodule's own
CLAUDE.md and treat it as additional project instructions.

**IMPORTANT -- do NOT run `git submodule update --init --recursive`
at the top level.** `mnexus/thirdparty/dawn` pulls in ~78 recursive
submodules (ANGLE references Google-internal hosts) and is NOT
needed: this app builds mnexus with the WebGPU/Dawn backend OFF.
Initialize instead:

```
git submodule update --init                 # top level
git -C mbase  submodule update --init --recursive
git -C mimage submodule update --init --recursive
git -C maudio submodule update --init --recursive
git -C mnexus submodule update --init thirdparty/SPIRV-Reflect thirdparty/VulkanMemoryAllocator
git -C mslang submodule update --init thirdparty/slang
git -C mslang/thirdparty/slang submodule update --init
```

## Build

Windows x64, MSVC (VS2022), CMake >= 3.22. **`cargo` (Rust) must
be on PATH**: mnexus builds `vidsynt` (H.265 NAL parser) and mgpmf
builds `gpmf_capi` from source via cargo at build time.

```
cmake -S . -B build-win_x64
cmake --build build-win_x64 --target routecam --config Debug
```

Run **from the repo root** (assets resolve relative to the cwd):

```
build-win_x64\Debug\routecam.exe [path\to\video.MP4]
```

The slang SPIR-V cache is written next to the cwd; first-run
shader compiles take a few seconds, later runs hit the cache.

## Testing

`ROUTECAM_AUTO_TRANSCODE=<scale>` (1 / 2 / 4) starts a transcode
of the loaded file as soon as playback is up and logs
`AUTO TRANSCODE finished` on completion -- lets scripts / agents
exercise the whole decode-compose-encode-mux path without driving
the UI. Logs go to `log/<timestamp>.txt` under the cwd (note: the
async logger loses its tail if the process is hard-killed; close
the window / WM_CLOSE for a clean flush).

## Known issues

- **GPU-contention chroma corruption (unresolved)**: running a
  transcode while another app uses the GPU heavily (e.g. browser
  video playback) can corrupt the output's chroma from the first
  IDR on (green / magenta blocks; the encode-input CbCr plane
  appears to reach the encoder unwritten). Suspected queue-
  ownership / residency race in the gfx->encode handoff (mnexus
  `feature/video` WIP territory). An abnormally large output file
  is the tell (e.g. 2.3 GB where a clean run gives 1.5 GB).
  Workaround: keep the GPU otherwise idle during transcodes.
- **gpmd / tmcd passthrough fails on HERO11 files**:
  `mmux::Mp4Muxer::AddPassthroughTrackByCodec` does not find the
  source tracks ("codec fourcc not found"), so transcoded output
  lacks telemetry / timecode tracks. Pre-existing; needs mmux
  investigation.
- **HDR10 sources cannot be transcoded** (playback is fine):
  mhevcenc is Main-profile 8-bit only so far.
- **`.360` transcode view is fixed** for the whole run (the
  playback view at Start); keyframed view animation is future
  work.

## OpenStreetMap usage

Tile fetches (both the async playback cache and the transcoder's
mosaic builder) send an identifying User-Agent and cache tiles
permanently under `%LOCALAPPDATA%\RouteCam\tiles` per the OSM tile
usage policy. Any UI or burned-in output that shows OSM tiles MUST
keep the "(c) OpenStreetMap" attribution.

## Commit messages

Conventional Commits, English, e.g. `feat: ...`, `fix: ...`,
`build: ...`. Scope is usually omitted (the repo IS the app);
use a submodule's name as the scope only when bumping it, e.g.
`chore: bump mnexus (...)`.
