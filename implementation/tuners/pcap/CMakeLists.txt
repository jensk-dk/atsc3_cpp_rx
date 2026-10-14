# atsc3::tuner_pcap — the pcap virtual tuner (spec 5.1.2) and the capture-file code it is built on: pcap/pcapng
# reader and writer, link-type stripping, format detector, DekTec .xa3 sidecar, BBP container.
add_library(atsc3_tuner_pcap STATIC
  pcap_reader.cpp
  link_strip.cpp
  bbp_container.cpp
  format_detector.cpp
  sidecar_xa3.cpp
  virtual_tuner.cpp)
add_library(atsc3::tuner_pcap ALIAS atsc3_tuner_pcap)
target_include_directories(atsc3_tuner_pcap
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_tuner_pcap PUBLIC atsc3::core PRIVATE pugixml::pugixml Threads::Threads)
atsc3_set_warnings(atsc3_tuner_pcap)
install(TARGETS atsc3_tuner_pcap EXPORT atsc3Targets ARCHIVE DESTINATION lib COMPONENT core)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include COMPONENT core)
