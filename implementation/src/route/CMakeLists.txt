# LCT, extension headers, FEC payload id, object store, completion, EFDT, delivery modes, packages (spec 5.8)
add_library(atsc3_route OBJECT route.cpp lct.cpp efdt.cpp multipart.cpp session.cpp)
target_include_directories(atsc3_route
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_route PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_route)
