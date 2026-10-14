# Golden expectations

The expected inventory of every corpus capture is asserted in `../scan_test.cpp` through `atsc3::tools::scanCapture`
(what `atsc3scan` prints): tuner and STLTP counters, BBPs per PLP and pointer facts, ALP packet types, LMTs, UDP flows
with byte counts, LLS tables, LCT flows per TSI/codepoint/HET, object completeness, MMTP flows per packet_id with
their messages and MP tables. The baselines of spec Section 9:

- BWI STLTP feed, first 320 000 tunnel packets (~144 s): 73 046 BBPs (PLP 0: 3424, PLP 1: 17 120, PLP 2: 52 502),
  282 642 ALP packets (280 930 IPv4, 1712 LMT), 2333 complete / 26 incomplete objects, 33 018 MMTP packets on WNUV,
  no sequence gaps.
- BWI, first 60 000 tunnel packets: 13 695 BBPs, 52 947 ALP packets, 461 / 26 objects, 6147 MMTP packets.
- UniSoft 3xROUTE, first 120 000 packets: 296 complete / 15 incomplete objects.
- UniSoft 2xROUTE+1MMT, first 120 000 packets: 142 / 5 objects, 49 108 MMTP packets.

Regenerate a number with `atsc3scan --limit N <capture>` and change the assertion in the same edit as the behaviour
that moved it.
