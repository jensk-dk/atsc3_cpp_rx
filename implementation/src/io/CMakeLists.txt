# Delivery units shared by the tuner contract and the layers (units.hpp) and the IPv4/UDP/RTP header helpers
# (inet.hpp). The capture-file code and the pcap virtual tuner live in tuners/pcap (spec 5.1).
add_library(atsc3_io OBJECT io.cpp inet.cpp)
target_include_directories(atsc3_io
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
atsc3_set_warnings(atsc3_io)
