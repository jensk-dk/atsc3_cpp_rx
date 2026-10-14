# Builds the Vue 3 frontend with npm and exposes frontend_dist for atsc3d to embed (spec 5.18.1).
find_program(NPM_EXECUTABLE npm REQUIRED)
set(FRONTEND_DIST ${CMAKE_CURRENT_SOURCE_DIR}/dist)
add_custom_command(
  OUTPUT ${FRONTEND_DIST}/index.html
  COMMAND ${NPM_EXECUTABLE} ci
  COMMAND ${NPM_EXECUTABLE} run build
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS package.json vite.config.ts index.html
  COMMENT "Building Vue 3 frontend")
add_custom_target(frontend_dist DEPENDS ${FRONTEND_DIST}/index.html)
