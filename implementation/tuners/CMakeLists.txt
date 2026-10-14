# Tuner implementations (spec 5.1): each implements service::ITuner over the core and is its own library that an
# application links and hands to Engine::open. The pcap virtual tuner is what every tool, test and integration replays
# captures with; vendor demodulator tuners are off by default because they need drivers.
if(ATSC3_TUNER_PCAP)
  add_subdirectory(pcap)
elseif(ATSC3_BUILD_TOOLS OR ATSC3_BUILD_TESTS OR ATSC3_WEB OR ATSC3_VLC)
  message(FATAL_ERROR "atsc3: the tools, tests, web server and VLC plugin replay captures and need ATSC3_TUNER_PCAP=ON")
endif()
if(ATSC3_TUNER_SONY)
  add_subdirectory(sony)
endif()
