# MMTP packet header, MPU/signaling payload headers, signaling reassembly, MP table, mmt_atsc3_message, MMT USBD (spec 5.10)
add_library(atsc3_mmtp OBJECT mmtp.cpp packet.cpp signaling.cpp session.cpp mpu.cpp)
target_include_directories(atsc3_mmtp
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_mmtp PRIVATE pugixml::pugixml ZLIB::ZLIB Threads::Threads)
atsc3_set_warnings(atsc3_mmtp)
