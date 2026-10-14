# libFuzzer targets, one per parser (spec 9, item 5). Built only with ATSC3_FUZZ=ON and clang.
foreach(target alp bbp stltp lct lls mmtp xml multipart)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_${target}.cpp)
    atsc3_add_fuzzer(fuzz_${target} fuzz_${target}.cpp)
  endif()
endforeach()
