# Integrations (spec Section 8): programs that embed the stack and present it through an interface of their own.
# Each is optional and consumes only the public API; nothing in src/ or tools/ depends on them.
#   web/  atsc3::web (cpp-httplib REST + media origins), the atsc3d daemon and the Vue 3 frontend   ATSC3_WEB
#   vlc/  atsc3_vlc_plugin, the VLC access-demux module                                               ATSC3_VLC
if(ATSC3_WEB)
  add_subdirectory(web)
endif()
if(ATSC3_VLC)
  add_subdirectory(vlc)
endif()
