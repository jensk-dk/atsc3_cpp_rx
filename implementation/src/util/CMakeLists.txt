# ByteView helpers, BitReader, Result, buffer pools, logging, clock/NTP, gzip (spec 8)
add_library(atsc3_util OBJECT util.cpp gzip.cpp)
target_include_directories(atsc3_util
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_util PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_util)
