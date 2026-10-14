# Test tree (spec Section 9). ctest labels: unit, golden, roundtrip, tools, http, bench, docs, e2e-web, e2e-vlc.
include(Catch)
include(Fuzzing)

# Documentation consistency (spec Section 8): the root README.md against the presets, options, program flags,
# MRL keys, test labels and names, npm scripts, Playwright projects and paths it names.
find_program(ATSC3_PYTHON3 python3)
if(ATSC3_PYTHON3)
  add_test(NAME docs.readme COMMAND ${ATSC3_PYTHON3} -I ${PROJECT_SOURCE_DIR}/scripts/readme-check.py ${PROJECT_SOURCE_DIR}/..)
  set_tests_properties(docs.readme PROPERTIES LABELS docs)
else()
  message(STATUS "atsc3: docs.readme skipped (python3 not found)")
endif()

add_subdirectory(support)
add_subdirectory(unit)
add_subdirectory(golden)
add_subdirectory(roundtrip)
add_subdirectory(fuzz)
add_subdirectory(bench)
if(ATSC3_WEB)
  add_subdirectory(http)
endif()
add_subdirectory(e2e)
