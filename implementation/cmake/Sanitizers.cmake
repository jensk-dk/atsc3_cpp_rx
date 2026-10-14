# ATSC3_SANITIZERS="address,undefined" adds the corresponding -fsanitize flags globally.
if(ATSC3_SANITIZERS)
  string(REPLACE "," ";" _san_list "${ATSC3_SANITIZERS}")
  foreach(_san IN LISTS _san_list)
    add_compile_options(-fsanitize=${_san} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_san})
  endforeach()
  message(STATUS "atsc3: sanitizers enabled: ${ATSC3_SANITIZERS}")
endif()
