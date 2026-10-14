# LLS table header, SignedMultiTable, SLT, SystemTime, AEAT, RRT, OMN, registry (spec 5.7)
add_library(atsc3_lls OBJECT lls.cpp processor.cpp parsers.cpp)
target_include_directories(atsc3_lls
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_lls PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_lls)
