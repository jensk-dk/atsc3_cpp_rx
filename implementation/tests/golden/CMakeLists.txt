# Golden tests: the atsc3scan inventory of every corpus capture asserted number by number (scan_test.cpp, spec 9
# item 2 and the Section 9 baselines). Registered only when the corpus directory exists.
add_executable(atsc3_golden golden_main.cpp scan_test.cpp)
target_link_libraries(atsc3_golden PRIVATE atsc3_test_support Catch2::Catch2WithMain)
atsc3_set_test_warnings(atsc3_golden)
if(EXISTS "${ATSC3_CORPUS_DIR}")
  catch_discover_tests(atsc3_golden PROPERTIES LABELS golden SKIP_RETURN_CODE 4)
else()
  message(STATUS "atsc3: corpus not found at ${ATSC3_CORPUS_DIR}; golden tests not registered")
endif()
