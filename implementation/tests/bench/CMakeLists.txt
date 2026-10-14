# Throughput benchmarks (spec 7): synthesized 60 Mbit/s stream and AsFast pcap replay.
add_executable(atsc3_bench bench_main.cpp)
target_link_libraries(atsc3_bench PRIVATE atsc3_test_support)
atsc3_set_warnings(atsc3_bench)
add_test(NAME bench.smoke COMMAND atsc3_bench --smoke)
set_tests_properties(bench.smoke PROPERTIES LABELS bench)
