# ISOBMFF helpers for the media paths (spec 5.10.3, 5.17.3/5.17.4): box walking, init-segment and moof inspection,
# RFC 6381 codecs strings, and the MSE rewrites of MMT MPU metadata / movie fragments.
add_library(atsc3_media OBJECT isobmff.cpp codecs.cpp)
target_include_directories(atsc3_media
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
atsc3_set_warnings(atsc3_media)
