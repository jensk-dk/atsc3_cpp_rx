# End-to-end suites (spec 5.18.3, 5.19.5 and 5.20.5), registered as ctest entries that call the scripts.
# They require the corpus and (web) node/playwright, (vlc) cvlc + python-vlc or (stb) the GStreamer plugins.
if(ATSC3_WEB)
  add_test(NAME e2e.web COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-web.sh $<TARGET_FILE:atsc3d> ${ATSC3_CORPUS_DIR})
  set_tests_properties(e2e.web PROPERTIES LABELS e2e-web DISABLED TRUE)
endif()
if(ATSC3_VLC)
  add_test(NAME e2e.vlc COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-vlc.sh $<TARGET_FILE:atsc3_vlc_plugin> ${ATSC3_CORPUS_DIR})
  # real-time replays of several captures (minutes); skipped (77) without a VLC runtime or the corpus
  set_tests_properties(e2e.vlc PROPERTIES LABELS e2e-vlc SKIP_RETURN_CODE 77 TIMEOUT 1200)
endif()
if(ATSC3_STB)
  add_test(NAME e2e.stb COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-stb.sh $<TARGET_FILE:atsc3stb> ${ATSC3_CORPUS_DIR})
  # headless real-time replays (about 2 minutes); skipped (77) without the corpus or the GStreamer plugins
  set_tests_properties(e2e.stb PROPERTIES LABELS e2e-stb SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()
