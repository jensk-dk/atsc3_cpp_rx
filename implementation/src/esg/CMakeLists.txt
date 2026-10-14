# ESG data model (spec 5.17.5): OMA BCAST SG Service/Schedule/Content fragments with the A/332 extensions and the
# programme index. The SGDU/SGDD containers are split in service/esg.cpp; this layer parses what comes out of them.
add_library(atsc3_esg OBJECT esg.cpp)
target_include_directories(atsc3_esg
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(atsc3_esg PRIVATE pugixml::pugixml)
atsc3_set_warnings(atsc3_esg)
