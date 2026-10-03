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

# ── Graphical build: compile whichever renderer backends are available. ──
set(CSIM_SOURCES src/main.c src/sim.c src/viz.c)
set(CSIM_DEFS "")
set(CSIM_LIBS m)

# raylib: use an installed copy if present, else fetch+build from source
# (no apt package needed; GL/X11 dev headers must be installed).
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
list(APPEND CSIM_SOURCES src/backend_raylib.c)
list(APPEND CSIM_DEFS HAVE_RAYLIB)
list(APPEND CSIM_LIBS raylib)
if(UNIX AND NOT APPLE)
  list(APPEND CSIM_LIBS GL X11 pthread dl rt)
endif()

# SDL2 (+ SDL2_ttf): only if installed via apt (libsdl2-dev libsdl2-ttf-dev).
find_package(PkgConfig QUIET)
set(HAVE_SDL2_BACKEND FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
  pkg_check_modules(SDL2TTF QUIET SDL2_ttf)
  if(SDL2_FOUND AND SDL2TTF_FOUND)
    set(HAVE_SDL2_BACKEND TRUE)
    list(APPEND CSIM_SOURCES src/backend_sdl2.c)
    list(APPEND CSIM_DEFS HAVE_SDL2)
    list(APPEND CSIM_LIBS ${SDL2_LIBRARIES} ${SDL2TTF_LIBRARIES})
  endif()
endif()

# ncurses (TUI): usually present; needs libncurses-dev.
set(HAVE_TUI_BACKEND FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(NCURSES QUIET ncursesw)
  if(NOT NCURSES_FOUND)
    pkg_check_modules(NCURSES QUIET ncurses)
  endif()
  if(NCURSES_FOUND)
    set(HAVE_TUI_BACKEND TRUE)
    list(APPEND CSIM_SOURCES src/backend_tui.c)
    list(APPEND CSIM_DEFS HAVE_TUI)
    list(APPEND CSIM_LIBS ${NCURSES_LIBRARIES})
  endif()
endif()

add_executable(csim ${CSIM_SOURCES})
target_compile_definitions(csim PRIVATE ${CSIM_DEFS})
target_link_libraries(csim ${CSIM_LIBS})
if(HAVE_SDL2_BACKEND)
  target_include_directories(csim PRIVATE ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
  target_link_directories(csim PRIVATE ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})
endif()
if(HAVE_TUI_BACKEND)
  target_include_directories(csim PRIVATE ${NCURSES_INCLUDE_DIRS})
endif()

message(STATUS "csim backends:  raylib=ON  sdl2=${HAVE_SDL2_BACKEND}  tui=${HAVE_TUI_BACKEND}")
