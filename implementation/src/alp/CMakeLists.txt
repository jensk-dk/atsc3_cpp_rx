# ALP header parser, segmentation, concatenation, TS encapsulation, dispatcher (spec 5.4)
add_library(atsc3_alp OBJECT alp.cpp header.cpp decap.cpp)
target_include_directories(atsc3_alp
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_alp PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_alp)
