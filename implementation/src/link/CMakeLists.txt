# LMT, RDT, PLP map, link signaling and the ROHC-U decompressor interface (spec 5.5)
# rohc/ will hold the in-house ROHC-U decompressor (profiles 1/2/4, U-mode), Phase 2.
add_library(atsc3_link OBJECT link.cpp lmt.cpp)
target_include_directories(atsc3_link
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_link PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_link)
