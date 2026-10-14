# lib/ is atsc3::tools, the library the programs are built from (the scan, routedump and stltp2alp bodies, packet
# builders, ALP→BBP and BBP→STLTP packers) and the tests link; the programs (spec 6.1) are one directory each, thin
# mains over lib/.
if(ATSC3_BUILD_TOOLS OR ATSC3_BUILD_TESTS)
  add_subdirectory(lib)
endif()
if(NOT ATSC3_BUILD_TOOLS)
  return()
endif()

# Corpus-driven smoke tests need the absolute sample path (ATSC3_CORPUS_DIR may be relative or unset here).
if(NOT ATSC3_CORPUS_DIR)
  set(ATSC3_CORPUS_DIR "${PROJECT_SOURCE_DIR}/../samples")
endif()
get_filename_component(ATSC3_CORPUS_DIR "${ATSC3_CORPUS_DIR}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
add_subdirectory(atsc3scan)
add_subdirectory(stltp2alp)
add_subdirectory(routedump)
