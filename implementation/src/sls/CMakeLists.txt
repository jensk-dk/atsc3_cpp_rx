# multipart parser (in route), metadata envelope, USBD, S-TSID, light MPD, HELD, SLS bundle, per-service
# ROUTE receiver (spec 5.9)
add_library(atsc3_sls OBJECT sls.cpp fragments.cpp mpd.cpp bundle.cpp route_receiver.cpp)
target_include_directories(atsc3_sls
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_sls PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_sls)
