cmake_minimum_required(VERSION 3.16)
project(emergent_city_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CORE_SOURCES src/sim.c src/systems.c src/world.c)

# ── Headless core: zero external dependencies (libm only). Always builds. ──
add_executable(csim_headless src/main_headless.c ${CORE_SOURCES})
target_link_libraries(csim_headless m)

# ── Graphical build: compile whichever renderer backends are available. ──
set(CSIM_SOURCES src/main.c src/viz.c ${CORE_SOURCES})
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

# notcurses (terminal renderer: pixel / sextant / ascii): needs libnotcurses-dev.
find_package(PkgConfig QUIET)
set(HAVE_NC_BACKEND FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(NOTCURSES QUIET notcurses)
  if(NOTCURSES_FOUND)
    set(HAVE_NC_BACKEND TRUE)
    list(APPEND CSIM_SOURCES src/backend_notcurses.c)
    list(APPEND CSIM_DEFS HAVE_NC)
    list(APPEND CSIM_LIBS ${NOTCURSES_LIBRARIES})
  endif()
endif()

add_executable(csim ${CSIM_SOURCES})
target_compile_definitions(csim PRIVATE ${CSIM_DEFS})
target_link_libraries(csim ${CSIM_LIBS})
if(HAVE_NC_BACKEND)
  target_include_directories(csim PRIVATE ${NOTCURSES_INCLUDE_DIRS})
  target_link_directories(csim PRIVATE ${NOTCURSES_LIBRARY_DIRS})
endif()

message(STATUS "csim backends:  raylib=ON  notcurses=${HAVE_NC_BACKEND}")
