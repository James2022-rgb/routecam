#
# RouteCam -- dedicated viewer / transcoder app for action-camera
# (GoPro HERO / Max 2) video files with GPS + IMU telemetry.
#
# Top-level CMake project that produces `routecam.exe`. Brings up its
# own SDL2 + Vulkan main loop on `mshell`, hosts a
# `routecam::RouteCamFrame` (= `mshell::IFrame`). Does NOT depend on
# `wentos-app2` -- this directory tree is structured so it can move
# to its own repo with mbase / mnexus / mplay / mshell / etc. pulled
# in as submodules.
#
# Build:
#   cmake -S routecam -B routecam/build-win_x64
#   cmake --build routecam/build-win_x64 --target routecam --config Debug
#

CMAKE_MINIMUM_REQUIRED(VERSION 3.22)

SET(CMAKE_CXX_STANDARD 23)
SET(CMAKE_CXX_STANDARD_REQUIRED True)

set(TARGET_NAME "routecam")

project(${TARGET_NAME}
  VERSION 0.1.0.0
  LANGUAGES C CXX
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ----- Sibling libs -----
# Order matters; downstream libs assert that their deps already exist.

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mbase  ${CMAKE_CURRENT_BINARY_DIR}/mbase)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mmath  ${CMAKE_CURRENT_BINARY_DIR}/mmath)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../masset ${CMAKE_CURRENT_BINARY_DIR}/masset)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mimage ${CMAKE_CURRENT_BINARY_DIR}/mimage)

# SDL2-static -- needed by mshell (ImGui SDL2 backend) and by this
# app's main loop. Source lives under app/thirdparty for now (will
# move to a top-level thirdparty/ once routecam is split off into
# its own repo).
if(NOT TARGET SDL2-static AND NOT ANDROID AND NOT EMSCRIPTEN)
  set(SDL2_DISABLE_SDL2MAIN ON)
  set(SDL2_DISABLE_UNINSTALL ON)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../app/thirdparty/SDL2-2.30.2 ${CMAKE_CURRENT_BINARY_DIR}/SDL2-2.30.2)
  foreach(t sdl_headers_copy SDL2-static)
    if(TARGET ${t})
      set_target_properties(${t} PROPERTIES FOLDER "external/SDL2")
    endif()
  endforeach()
endif()

# mnexus -- Vulkan abstraction with Vulkan Video on Windows.
if(WIN32)
  set(MNEXUS_ENABLE_VIDEO_CODING ON CACHE BOOL "" FORCE)
endif()
# routecam (via mshell) hard-selects the Vulkan backend. Disable the
# WebGPU/Dawn backend so the build does not pull in Dawn and its ~78
# recursive submodules -- a large chunk of configure + build time we
# gain nothing from here.
set(MNEXUS_ENABLE_BACKEND_WGPU OFF CACHE BOOL "" FORCE)
set(MNEXUS_ENABLE_DAWN         OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mnexus       ${CMAKE_CURRENT_BINARY_DIR}/mnexus)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mslang       ${CMAKE_CURRENT_BINARY_DIR}/mslang)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mslang-proxy ${CMAKE_CURRENT_BINARY_DIR}/mslang-proxy)

# Media stack used by RouteCamFrame.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../maudio   ${CMAKE_CURRENT_BINARY_DIR}/maudio)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mdemux   ${CMAKE_CURRENT_BINARY_DIR}/mdemux)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mgpmf    ${CMAKE_CURRENT_BINARY_DIR}/mgpmf)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../maacdec  ${CMAKE_CURRENT_BINARY_DIR}/maacdec)
if(WIN32)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mhevcdec ${CMAKE_CURRENT_BINARY_DIR}/mhevcdec)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mplay    ${CMAKE_CURRENT_BINARY_DIR}/mplay)
endif()

# mshell (window + mnexus + ImGui + per-frame IFrame lifecycle).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mshell ${CMAKE_CURRENT_BINARY_DIR}/mshell)

# ----- routecam.exe -----

set(SOURCE_DIR "src")
set(SOURCES
  ${SOURCE_DIR}/gps_timeline.cpp
  ${SOURCE_DIR}/gps_timeline.h
  ${SOURCE_DIR}/main.cpp
  ${SOURCE_DIR}/minimap.cpp
  ${SOURCE_DIR}/minimap.h
  ${SOURCE_DIR}/osm_tile_cache.cpp
  ${SOURCE_DIR}/osm_tile_cache.h
  ${SOURCE_DIR}/routecam_frame.h
  ${SOURCE_DIR}/routecam_frame.cpp
  ${SOURCE_DIR}/slippy_map.h
)

add_executable(${TARGET_NAME})
target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

set_target_properties(${TARGET_NAME} PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
)

target_sources(${TARGET_NAME} PRIVATE ${SOURCES})

target_link_libraries(${TARGET_NAME} PRIVATE
  mbase
  mgpmf
  mimage
  mshell
  mplay
)

if(WIN32)
  # WinHTTP for the OSM tile fetcher.
  target_link_libraries(${TARGET_NAME} PRIVATE winhttp)
endif()

if(WIN32)
  GET_PROPERTY(mslang_dependent_dll_files GLOBAL PROPERTY MSLANG_DEPENDENT_DLL_FILES_PROPERTY)

  ADD_CUSTOM_COMMAND(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:${TARGET_NAME}> $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND ${CMAKE_COMMAND} -E copy ${mslang_dependent_dll_files} $<TARGET_FILE_DIR:${TARGET_NAME}>
    COMMAND_EXPAND_LISTS
  )
endif()
