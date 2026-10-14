# Third-party dependencies. System packages are preferred; FetchContent (pinned) is the fallback
# when ATSC3_FETCH_DEPS=ON. Versions are mirrored in third_party/README.md.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# --- pugixml (XML) ---
find_package(pugixml 1.12 QUIET CONFIG)
if(NOT pugixml_FOUND)
  if(NOT ATSC3_FETCH_DEPS)
    message(FATAL_ERROR "pugixml not found and ATSC3_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG v1.14
    GIT_SHALLOW TRUE)
  set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(pugixml)
  if(NOT TARGET pugixml::pugixml)
    add_library(pugixml::pugixml ALIAS pugixml-static)
  endif()
endif()

# --- Catch2 (tests) ---
if(ATSC3_BUILD_TESTS)
  find_package(Catch2 3 QUIET CONFIG)
  if(NOT Catch2_FOUND)
    if(NOT ATSC3_FETCH_DEPS)
      message(WARNING "Catch2 not found and ATSC3_FETCH_DEPS=OFF: tests disabled")
      set(ATSC3_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    else()
      FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE)
      FetchContent_MakeAvailable(Catch2)
      list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
  endif()
endif()

# --- OpenSSL (security module) ---
if(ATSC3_SECURITY)
  find_package(OpenSSL 1.1 REQUIRED)
endif()

# --- libpcap (live capture only, optional) ---
if(PkgConfig_FOUND)
  pkg_check_modules(PCAP QUIET IMPORTED_TARGET libpcap)
endif()

# --- web server deps ---
if(ATSC3_WEB)
  find_package(httplib QUIET CONFIG)
  if(NOT httplib_FOUND)
    FetchContent_Declare(httplib
      GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
      GIT_TAG v0.15.3
      GIT_SHALLOW TRUE)
    set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(httplib)
  endif()
  # IXWebSocket (binary media WebSocket, spec 5.17.4) is added together with the MSE push player.
endif()
