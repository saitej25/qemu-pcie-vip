cmake_minimum_required(VERSION 3.16)
project(mini_ics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# mini_ics_core is linked both into plain executables and into the
# mini_ics_dpi shared library; building it PIC unconditionally is
# simpler and cheaper than maintaining two build variants.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: protocol, transport, transaction correlation, logging.
# Everything else (server, host client, DPI shim, tests) links against
# this single static library so there is exactly one build of the wire
# format and socket code.
# ---------------------------------------------------------------------------
add_library(mini_ics_core STATIC
  src/protocol.cpp
  src/socket.cpp
  src/transaction.cpp
  src/logger.cpp
)
target_include_directories(mini_ics_core PUBLIC include)
target_link_libraries(mini_ics_core PUBLIC Threads::Threads)

# Server logic as its own object library so both the standalone
# mini_ics_server executable and the DPI shim can link the same
# MiniIcsServer implementation without duplicating compilation.
add_library(mini_ics_server_lib STATIC src/server.cpp)
target_link_libraries(mini_ics_server_lib PUBLIC mini_ics_core)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(mini_ics_server src/server.cpp)
target_compile_definitions(mini_ics_server PRIVATE MINI_ICS_BUILD_SERVER_MAIN)
target_link_libraries(mini_ics_server PRIVATE mini_ics_core)

add_executable(mini_ics_host_client src/host_client.cpp)
target_link_libraries(mini_ics_host_client PRIVATE mini_ics_core)

# ---------------------------------------------------------------------------
# DPI-C shared library, for QuestaSim / Verilator to load.
# Only built when a SystemVerilog toolchain that can consume a DPI .so is
# expected to be present; the C++ side always builds so protocol/socket
# tests work even without any HDL simulator installed.
# ---------------------------------------------------------------------------
option(MINI_ICS_BUILD_DPI "Build the DPI-C shared library for SV simulators" ON)
if(MINI_ICS_BUILD_DPI)
  # svdpi.h ships with the SV simulator, not with a standard compiler
  # include path. Look in the usual QuestaSim location plus anything the
  # caller points MINI_ICS_SVDPI_INCLUDE_DIR / $VSIM_HOME at; if not
  # found, the DPI target is skipped with a clear message rather than
  # failing the whole configure (the C++ core and tests must still build
  # without any HDL simulator installed).
  find_path(MINI_ICS_SVDPI_INCLUDE_DIR svdpi.h
    HINTS
      ENV VSIM_HOME
      ENV QUESTA_HOME
      ${MINI_ICS_SVDPI_INCLUDE_DIR}
    PATHS
      /tools/questa/questasim/include
    PATH_SUFFIXES include
  )

  if(MINI_ICS_SVDPI_INCLUDE_DIR)
    message(STATUS "Building mini_ics_dpi with svdpi.h from ${MINI_ICS_SVDPI_INCLUDE_DIR}")
    add_library(mini_ics_dpi SHARED
      dpi/mini_ics_dpi.cpp
      src/server.cpp
    )
    target_include_directories(mini_ics_dpi PRIVATE dpi ${MINI_ICS_SVDPI_INCLUDE_DIR})
    target_link_libraries(mini_ics_dpi PRIVATE mini_ics_core)

    # QuestaSim's `vsim` process bundles its own (older) libstdc++ and
    # loads it before this .so, which then fails to satisfy the newer
    # GLIBCXX symbol versions the host system compiler emits by default
    # (observed: "GLIBCXX_3.4.29 not found"). Statically linking
    # libstdc++/libgcc into the DPI shared object avoids depending on
    # whichever libstdc++ the *host simulator process* happens to have
    # loaded first; it does not affect any other target.
    target_link_options(mini_ics_dpi PRIVATE -static-libstdc++ -static-libgcc)
  else()
    message(STATUS "svdpi.h not found; skipping mini_ics_dpi target "
                    "(set MINI_ICS_SVDPI_INCLUDE_DIR to point at a simulator's include dir)")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
