# atsc3scan — capture inventory: format verdict, link-layer facts, LLS tables, ROUTE/LCT flows with object
# completeness, MMTP flows and signaling, UDP flow table (spec 6.1). The numbers it reports on the corpus are
# asserted by tests/golden/scan_test.cpp; these smoke tests only check that it runs and finds the content.
add_executable(atsc3scan main.cpp)
target_link_libraries(atsc3scan PRIVATE atsc3::tools)
atsc3_set_warnings(atsc3scan)
install(TARGETS atsc3scan RUNTIME DESTINATION bin COMPONENT tools)

if(EXISTS "${ATSC3_CORPUS_DIR}/ch11-25-slt.pcap")
  add_test(NAME tools.atsc3scan.ch11 COMMAND atsc3scan --slt --objs "${ATSC3_CORPUS_DIR}/ch11-25-slt.pcap")
  set_tests_properties(tools.atsc3scan.ch11 PROPERTIES LABELS "golden;tools" PASS_REGULAR_EXPRESSION "objects: complete=6 incomplete=16")
endif()
if(EXISTS "${ATSC3_CORPUS_DIR}/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap")
  add_test(NAME tools.atsc3scan.bwi COMMAND atsc3scan --limit 60000 --mmt "${ATSC3_CORPUS_DIR}/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap")
  set_tests_properties(tools.atsc3scan.bwi PROPERTIES LABELS "golden;tools" PASS_REGULAR_EXPRESSION "objects: complete=461 incomplete=26")
endif()
