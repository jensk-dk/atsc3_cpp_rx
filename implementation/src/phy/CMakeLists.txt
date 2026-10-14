# Baseband Packet header parser and deframer; descrambler to follow (spec 5.3)
add_library(atsc3_phy OBJECT phy.cpp bbp_deframer.cpp)
target_include_directories(atsc3_phy
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_phy PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_phy)
