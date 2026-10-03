cmake_minimum_required(VERSION 3.16)
project(emergent_city_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Headless core: zero external dependencies (libm only). Always builds. ──
add_executable(csim_headless src/main_headless.c src/sim.c)
target_link_libraries(csim_headless m)

# ── Graphics build (raylib). Use an installed raylib if present, otherwise
#    fetch and build it from source (no apt package needed; the GL/X11 dev
#    headers it needs are already on the system). ──
find_package(raylib 4.5 QUIET)
if(NOT raylib_FOUND)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(raylib)
endif()

add_executable(csim src/main.c src/sim.c)
target_link_libraries(csim raylib m)
if(UNIX AND NOT APPLE)
  target_link_libraries(csim GL X11 pthread dl rt)
endif()
