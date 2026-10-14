add_executable(routedump main.cpp)
target_link_libraries(routedump PRIVATE atsc3::tools)
atsc3_set_warnings(routedump)
install(TARGETS routedump RUNTIME DESTINATION bin COMPONENT tools)

# Corpus smoke tests: every service of the Baltimore STLTP feed (first 8 s) and the UniSoft raw-IP capture.
if(EXISTS "${ATSC3_CORPUS_DIR}/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap")
  add_test(NAME tools.routedump.bwi
    COMMAND routedump --duration 8 "${ATSC3_CORPUS_DIR}/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap"
            "${CMAKE_CURRENT_BINARY_DIR}/bwi-8s")
  set_tests_properties(tools.routedump.bwi PROPERTIES LABELS "golden;tools")
endif()
if(EXISTS "${ATSC3_CORPUS_DIR}/dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap")
  add_test(NAME tools.routedump.unisoft
    COMMAND routedump --duration 12 "${ATSC3_CORPUS_DIR}/dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap"
            "${CMAKE_CURRENT_BINARY_DIR}/unisoft-12s")
  set_tests_properties(tools.routedump.unisoft PROPERTIES LABELS "golden;tools")
endif()
