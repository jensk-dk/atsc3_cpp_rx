# pugixml wrapper: namespace-tolerant lookup, xs:dateTime/duration/list parsing, raw extension capture (spec 5.16)
add_library(atsc3_xml OBJECT xml.cpp)
target_include_directories(atsc3_xml
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_xml PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_xml)
