# CTP outer tunnel, inner reframer, BPPS reassembly, ALPTP; Preamble/L1 and T&M parsers to follow (spec 5.2)
add_library(atsc3_stltp OBJECT stltp.cpp demux.cpp)
target_include_directories(atsc3_stltp
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_stltp PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_stltp)
