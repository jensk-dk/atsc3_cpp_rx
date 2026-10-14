# atsc3::tools — the programs' bodies (capture inventory, STLTP→ALP conversion, ROUTE service extraction) and the
# packet builders (IP/UDP/RTP/Ethernet), ALP→BBP and BBP→STLTP packers; linked by the thin mains and the tests (spec 6, 9).
add_library(atsc3tools STATIC tools.cpp packets.cpp bbp_packer.cpp stltp_packer.cpp stltp2alp.cpp routedump.cpp scan.cpp)
add_library(atsc3::tools ALIAS atsc3tools)
target_include_directories(atsc3tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(atsc3tools PUBLIC atsc3::core atsc3::tuner_pcap)
atsc3_set_warnings(atsc3tools)
