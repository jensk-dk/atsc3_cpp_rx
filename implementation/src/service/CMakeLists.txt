# Service engine: chain runner, service table from all SLTs, SLS acquisition per service, component model,
# selection, snapshots (spec 5.11)
add_library(atsc3_service OBJECT service.cpp engine.cpp esg.cpp tuner.cpp)
target_include_directories(atsc3_service
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_service PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_service)
