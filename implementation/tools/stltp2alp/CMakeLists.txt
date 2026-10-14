add_executable(stltp2alp main.cpp)
target_link_libraries(stltp2alp PRIVATE atsc3::tools)
atsc3_set_warnings(stltp2alp)
install(TARGETS stltp2alp RUNTIME DESTINATION bin COMPONENT tools)

# Smoke test on the Baltimore STL capture when the corpus is present (first 2 s).
if(EXISTS "${ATSC3_CORPUS_DIR}/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap")
  add_test(NAME tools.stltp2alp.bwi
    COMMAND stltp2alp --duration 2 "${ATSC3_CORPUS_DIR}/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap"
            "${CMAKE_CURRENT_BINARY_DIR}/bwi-2s.alp.pcap")
  set_tests_properties(tools.stltp2alp.bwi PROPERTIES LABELS "golden;tools")
endif()
