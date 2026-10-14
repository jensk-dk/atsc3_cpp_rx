# Tuners

Each tuner implements `atsc3::service::ITuner` (spec 5.1, `include/atsc3/service/tuner.hpp`) and delivers
Baseband Packets, ALP packets or IP datagrams; it states which of those it can deliver and the engine couples
it to the receive chain at that layer. `pcap/` is the virtual tuner over capture files that every tool, test
and integration uses (`ATSC3_TUNER_PCAP`, on by default). RF tuners are optional targets, off by default,
because they need vendor drivers; the Sony demodulator (`ATSC3_TUNER_SONY`) is the first, output format TBD.
