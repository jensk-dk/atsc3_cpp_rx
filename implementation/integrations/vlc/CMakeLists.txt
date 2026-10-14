# VLC integration (spec 5.19): access-demux module "atsc3://": the VLC 3.0 plugin headers come from an installed vlc-plugin.pc or
# from a VLC source tree beside the repository (cmake/FindVLCPlugin.cmake); the stack is linked in statically.
include(FindVLCPlugin)

# VLC-free MRL/option parsing, unit-tested by atsc3_unit
add_library(atsc3_vlc_mrl STATIC src/mrl.cpp)
target_include_directories(atsc3_vlc_mrl PUBLIC src)
atsc3_set_warnings(atsc3_vlc_mrl)

add_library(atsc3_vlc_plugin MODULE src/module.cpp src/demux.cpp src/es_mapper.cpp src/epg.cpp src/alerts.cpp)
set_target_properties(atsc3_vlc_plugin PROPERTIES OUTPUT_NAME "atsc3_plugin" PREFIX "lib")
target_link_libraries(atsc3_vlc_plugin PRIVATE atsc3_vlc_mrl atsc3::core atsc3::tuner_pcap VLC::plugin)
# VLC's headers and macros are C (old-style casts, implicit conversions): the first-party set minus those three
if(NOT MSVC)
  target_compile_options(atsc3_vlc_plugin PRIVATE -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
                         -Wno-old-style-cast -Wno-sign-conversion -Wno-conversion)
endif()
install(TARGETS atsc3_vlc_plugin LIBRARY DESTINATION "${VLC_PLUGINSDIR}/access" COMPONENT vlc-plugin)
