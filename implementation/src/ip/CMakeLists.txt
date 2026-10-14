# IPv4, fragment reassembly, UDP, flow router (spec 5.6)
add_library(atsc3_ip OBJECT ip.cpp demux.cpp)
target_include_directories(atsc3_ip
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_ip PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_ip)
