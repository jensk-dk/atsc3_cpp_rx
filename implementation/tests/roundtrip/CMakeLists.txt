# Converter round trips (spec 9, item 3): alp -> bbp -> stltp pcap -> stltp2alp -> identical ALP sequence.
add_executable(atsc3_roundtrip roundtrip_main.cpp)
target_link_libraries(atsc3_roundtrip PRIVATE atsc3_test_support Catch2::Catch2WithMain)
atsc3_set_test_warnings(atsc3_roundtrip)
catch_discover_tests(atsc3_roundtrip PROPERTIES LABELS roundtrip)
