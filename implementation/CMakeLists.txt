cmake_minimum_required(VERSION 3.21)
project(atsc3 VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "ATSC 3.0 client-side stack")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(Options)
include(CompilerWarnings)
include(Sanitizers)
include(Coverage)
include(Dependencies)

if(ATSC3_BUILD_TESTS)
  enable_testing()  # before tools/: their add_test() calls would otherwise be dropped silently
endif()
add_subdirectory(src)
add_subdirectory(tuners)  # tuner implementations over the core: the pcap virtual tuner (ATSC3_TUNER_PCAP), vendor demodulators
add_subdirectory(tools)   # tools/lib = atsc3::tools (tools or tests) + the standalone programs (ATSC3_BUILD_TOOLS)
add_subdirectory(integrations)   # web server + daemon + frontend (ATSC3_WEB), VLC plugin (ATSC3_VLC)
if(ATSC3_BUILD_TESTS)
  add_subdirectory(tests)
endif()

include(Packaging)
