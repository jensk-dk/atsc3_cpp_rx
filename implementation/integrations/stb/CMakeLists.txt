# STB integration (spec 5.20): atsc3stb — the stack on the pcap virtual tuner, GStreamer A/V decoding in a window,
# a channel list and a channel banner drawn with cairo. Needs the GStreamer 1.20+ development packages
# (gstreamer-1.0, gstreamer-app-1.0, gstreamer-video-1.0) and cairo; at run time the base, good and libav plugins
# (compositor, audiomixer, cairooverlay, decodebin, avdec_h265/avdec_aac, the auto sinks).
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.20 gstreamer-app-1.0 gstreamer-video-1.0 cairo)

# GStreamer-free UI model (channel list, keys, banner timer, present/following), unit-tested by atsc3_unit
add_library(atsc3_stb_model STATIC src/model.cpp)
target_include_directories(atsc3_stb_model PUBLIC src)
target_link_libraries(atsc3_stb_model PUBLIC atsc3::core)
atsc3_set_warnings(atsc3_stb_model)

add_executable(atsc3stb src/main.cpp src/player.cpp src/overlay.cpp)
target_link_libraries(atsc3stb PRIVATE atsc3_stb_model atsc3::core atsc3::tuner_pcap PkgConfig::GST)
target_include_directories(atsc3stb SYSTEM PRIVATE ${GST_INCLUDE_DIRS})
# GLib/GStreamer headers and macros are C (old-style casts, implicit conversions): the first-party set minus those
if(NOT MSVC)
  target_compile_options(atsc3stb PRIVATE -Wall -Wextra -Werror -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
                         -Wno-old-style-cast -Wno-sign-conversion -Wno-conversion)
endif()
install(TARGETS atsc3stb RUNTIME DESTINATION bin COMPONENT stb)
