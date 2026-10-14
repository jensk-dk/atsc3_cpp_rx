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
