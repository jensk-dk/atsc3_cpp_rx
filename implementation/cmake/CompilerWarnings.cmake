# atsc3_set_warnings(<target>): strict warnings, warnings as errors, for every first-party target.
function(atsc3_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor
      -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
  endif()
endfunction()

# atsc3_set_test_warnings(<target>): the same set for a test target, minus GCC's optimizer-dependent late warnings.
# -Wnull-dereference, -Wstringop-overread and -Wstringop-overflow only fire at -O2/-O3 and GCC 11 raises them on the
# idioms test code is made of (vector::insert on a literal-sized vector, operator[] right after copying a vector,
# REQUIRE(p) followed by p->x), so the debug preset never sees them and the release preset fails with -Werror. The
# library, tools and integrations keep the full set; their sources are clean at -O3.
function(atsc3_set_test_warnings target)
  atsc3_set_warnings(${target})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE -Wno-null-dereference -Wno-stringop-overread -Wno-stringop-overflow)
  endif()
endfunction()
