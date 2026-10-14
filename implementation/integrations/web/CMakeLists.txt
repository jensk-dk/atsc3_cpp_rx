# Web integration (spec 5.17): atsc3::web — embedded HTTP server (cpp-httplib): REST API over service::Engine, capture listing, static
# frontend serving (spec 5.17). The WebSocket media push (IXWebSocket) arrives with the MSE player.
add_library(atsc3web STATIC web.cpp json.cpp server.cpp)
add_library(atsc3::web ALIAS atsc3web)
target_include_directories(atsc3web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(atsc3web PUBLIC atsc3::core atsc3::tuner_pcap PRIVATE httplib::httplib Threads::Threads)
target_compile_definitions(atsc3web PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=4)
atsc3_set_warnings(atsc3web)
# cpp-httplib is header-only and not warning-clean under -Wpedantic/-Wconversion: treat it as a system header.
get_target_property(_httplib_inc httplib::httplib INTERFACE_INCLUDE_DIRECTORIES)
if(_httplib_inc)
  target_include_directories(atsc3web SYSTEM PRIVATE ${_httplib_inc})
endif()

# The frontend (npm, only with ATSC3_FRONTEND) must come before atsc3d so that atsc3d can depend on frontend_dist.
if(ATSC3_FRONTEND)
  add_subdirectory(frontend)
endif()
add_subdirectory(atsc3d)
