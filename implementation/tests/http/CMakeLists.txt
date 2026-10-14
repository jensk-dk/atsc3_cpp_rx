# REST contract tests against web::Server without a browser (spec 9, item 10).
add_executable(atsc3_http http_main.cpp)
target_link_libraries(atsc3_http PRIVATE atsc3_test_support atsc3::web httplib::httplib Catch2::Catch2WithMain)
get_target_property(_httplib_inc httplib::httplib INTERFACE_INCLUDE_DIRECTORIES)
if(_httplib_inc)
  target_include_directories(atsc3_http SYSTEM PRIVATE ${_httplib_inc})
endif()
atsc3_set_test_warnings(atsc3_http)
catch_discover_tests(atsc3_http PROPERTIES LABELS http SKIP_RETURN_CODE 4)
