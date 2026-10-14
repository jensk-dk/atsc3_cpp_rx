# One OBJECT library per layer (spec Section 8), all combined into atsc3core / atsc3::core.
set(ATSC3_LAYERS util xml io stltp phy alp link ip lls route sls media mmtp service esg)
if(ATSC3_SECURITY)
  list(APPEND ATSC3_LAYERS security)
endif()

foreach(layer IN LISTS ATSC3_LAYERS)
  add_subdirectory(${layer})
endforeach()

add_library(atsc3core STATIC stack.cpp version.cpp)
add_library(atsc3::core ALIAS atsc3core)
foreach(layer IN LISTS ATSC3_LAYERS)
  # Objects are absorbed into the archive (keeps the export set free of the per-layer targets).
  target_sources(atsc3core PRIVATE $<TARGET_OBJECTS:atsc3_${layer}>)
endforeach()
target_include_directories(atsc3core
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(atsc3core PRIVATE
  ATSC3_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  ATSC3_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  ATSC3_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  ATSC3_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(atsc3core PUBLIC Threads::Threads PRIVATE ZLIB::ZLIB pugixml::pugixml)
if(ATSC3_SECURITY)
  target_link_libraries(atsc3core PRIVATE OpenSSL::Crypto)
  target_compile_definitions(atsc3core PUBLIC ATSC3_HAVE_SECURITY=1)
endif()
atsc3_set_warnings(atsc3core)

install(TARGETS atsc3core EXPORT atsc3Targets
  ARCHIVE DESTINATION lib COMPONENT core
  LIBRARY DESTINATION lib COMPONENT core)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION include COMPONENT core)
install(EXPORT atsc3Targets NAMESPACE atsc3:: DESTINATION lib/cmake/atsc3 COMPONENT core)
include(CMakePackageConfigHelpers)
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/atsc3Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/atsc3Config.cmake
  INSTALL_DESTINATION lib/cmake/atsc3)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/atsc3ConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/atsc3Config.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/atsc3ConfigVersion.cmake
        DESTINATION lib/cmake/atsc3 COMPONENT core)
