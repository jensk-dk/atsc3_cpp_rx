# CDT certificate store and CMS signature verification with OpenSSL (spec 5.14)
add_library(atsc3_security OBJECT security.cpp)
target_include_directories(atsc3_security
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_security PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_security)