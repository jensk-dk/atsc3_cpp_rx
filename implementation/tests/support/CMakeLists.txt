# Shared test helpers: packet builders, recording tuner sink, corpus lookup.
add_library(atsc3_test_support STATIC corpus.cpp packets.cpp)
target_include_directories(atsc3_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_test_support PUBLIC atsc3::core atsc3::tuner_pcap atsc3::tools pugixml::pugixml ZLIB::ZLIB)
target_compile_definitions(atsc3_test_support PUBLIC
  ATSC3_CORPUS_DIR="${ATSC3_CORPUS_DIR}"
  ATSC3_SCHEMA_DIR="${ATSC3_SCHEMA_DIR}"
  ATSC3_VECTOR_DIR="${CMAKE_SOURCE_DIR}/tests/vectors")
atsc3_set_warnings(atsc3_test_support)
