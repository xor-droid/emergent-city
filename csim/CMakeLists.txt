cmake_minimum_required(VERSION 3.16)
project(emergent_city_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig QUIET)

# ── LLM layer: libcurl + cJSON (+pthread). Optional; sim is rule-based without. ──
set(HAVE_LLM_LIBS FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(CURL QUIET libcurl)
  pkg_check_modules(CJSON QUIET libcjson)
  if(CURL_FOUND AND CJSON_FOUND)
    set(HAVE_LLM_LIBS TRUE)
  endif()
endif()

# Applies the LLM build settings to a target (llm.c is always compiled; without
# the libs it is a no-op stub).
function(apply_llm tgt)
  if(HAVE_LLM_LIBS)
    target_compile_definitions(${tgt} PRIVATE HAVE_LLM)
    target_include_directories(${tgt} PRIVATE ${CURL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
    target_link_directories(${tgt} PRIVATE ${CURL_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})
    target_link_libraries(${tgt} ${CURL_LIBRARIES} ${CJSON_LIBRARIES} pthread)
  endif()
endfunction()

set(CORE_SOURCES src/sim.c src/systems.c src/world.c src/llm.c)

# ── Headless core (libm + optional LLM). Always builds. ──
add_executable(csim_headless src/main_headless.c ${CORE_SOURCES})
target_link_libraries(csim_headless m)
apply_llm(csim_headless)

# ── Graphical build: compile whichever renderer backends are available. ──
set(CSIM_SOURCES src/main.c src/viz.c ${CORE_SOURCES})
set(CSIM_DEFS "")
set(CSIM_LIBS m)

# raylib: use an installed copy if present, else fetch+build from source.
find_package(raylib 4.5 QUIET)
if(NOT raylib_FOUND)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
endif()
list(APPEND CSIM_SOURCES src/backend_raylib.c)
list(APPEND CSIM_DEFS HAVE_RAYLIB)
list(APPEND CSIM_LIBS raylib)
if(UNIX AND NOT APPLE)
  list(APPEND CSIM_LIBS GL X11 pthread dl rt)
endif()

# notcurses (terminal renderer).
#   - Default: use the system libnotcurses-dev via pkg-config.
#   - CSIM_FETCH_NOTCURSES=ON: build notcurses from source in Release mode. This
#     avoids the Debian 3.0.17 input-parser assertion (process_escape / in.c)
#     that aborts on Kitty-protocol terminals — a Release build defines NDEBUG,
#     which disables that assert. Needs: libunistring-dev libdeflate-dev.
option(CSIM_FETCH_NOTCURSES "Build notcurses from source (Release; avoids the 3.0.17 input assert)" OFF)
set(HAVE_NC_BACKEND FALSE)
set(NC_FETCHED FALSE)
if(CSIM_FETCH_NOTCURSES)
  include(FetchContent)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)   # NDEBUG disables the assert
  set(USE_MULTIMEDIA none CACHE STRING "" FORCE)
  set(USE_QRCODEGEN OFF CACHE BOOL "" FORCE)
  set(USE_CPP OFF CACHE BOOL "" FORCE)
  set(USE_PANDOC OFF CACHE BOOL "" FORCE)
  set(USE_DOCTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(notcurses
    GIT_REPOSITORY https://github.com/dankamongmen/notcurses.git
    GIT_TAG master GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(notcurses)
  set(HAVE_NC_BACKEND TRUE)
  set(NC_FETCHED TRUE)
  list(APPEND CSIM_SOURCES src/backend_notcurses.c)
  list(APPEND CSIM_DEFS HAVE_NC)
  list(APPEND CSIM_LIBS notcurses)
elseif(PkgConfig_FOUND)
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
if(HAVE_NC_BACKEND AND NOT NC_FETCHED)
  target_include_directories(csim PRIVATE ${NOTCURSES_INCLUDE_DIRS})
  target_link_directories(csim PRIVATE ${NOTCURSES_LIBRARY_DIRS})
endif()
apply_llm(csim)

message(STATUS "csim backends:  raylib=ON  notcurses=${HAVE_NC_BACKEND} (fetched=${NC_FETCHED})  llm=${HAVE_LLM_LIBS}")
