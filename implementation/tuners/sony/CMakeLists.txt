# Sony demodulator tuner (spec Q1): output (BBP / ALP / framed ALP) to be confirmed from the driver documentation.
# Links against the vendor driver; not built by default.
add_library(atsc3_tuner_sony STATIC sony_tuner.cpp)
add_library(atsc3::tuner_sony ALIAS atsc3_tuner_sony)
target_link_libraries(atsc3_tuner_sony PUBLIC atsc3::core)
atsc3_set_warnings(atsc3_tuner_sony)
