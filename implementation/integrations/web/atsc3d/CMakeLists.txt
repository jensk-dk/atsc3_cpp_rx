# atsc3d — daemon: stack + source (pcap virtual tuner / UDP / RF adapter) + the web server (--no-web runs headless).
add_executable(atsc3d main.cpp)
target_link_libraries(atsc3d PRIVATE atsc3::core atsc3::tuner_pcap atsc3::web)
target_compile_definitions(atsc3d PRIVATE ATSC3_HAVE_WEB=1)
get_filename_component(_atsc3d_captures "${ATSC3_CORPUS_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
get_filename_component(_atsc3d_www "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/dist" ABSOLUTE)
target_compile_definitions(atsc3d PRIVATE
  ATSC3D_DEFAULT_CAPTURES="${_atsc3d_captures}"
  ATSC3D_DEFAULT_WWW="${_atsc3d_www}")
if(ATSC3_FRONTEND AND TARGET frontend_dist)
  add_dependencies(atsc3d frontend_dist)
endif()
atsc3_set_warnings(atsc3d)
install(TARGETS atsc3d RUNTIME DESTINATION bin COMPONENT daemon)
