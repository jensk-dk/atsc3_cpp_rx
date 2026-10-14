# MMT / ISOBMFF vectors

Real on-air bytes (each < 2 kB) used by `tests/unit/media/`. Nothing here is synthetic.

MPU data units cut from the MMTP flows (one MPU per asset):

| file | source | what |
|---|---|---|
| `unisoft-20-init.mp4`, `unisoft-21-init.mp4` | `samples/dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap`, 239.255.24.3:8000, packet_id 20 (HEVC 1080p) / 21 (AAC-LC) | FT=0 MPU metadata DU: `ftyp(mpuf) mmpu moov` with the media trak and the MMT hint trak (`mmth`, trex default_sample_size 34), MPU sequence 996 |
| `unisoft-20-moof.m4s`, `unisoft-21-moof.m4s` | same, MPU 996 | FT=1 movie-fragment DU: `moof` (media traf + hint traf) + the 8-byte `mdat` header exactly as sent |
| `bwi-100-init.mp4`, `bwi-200-init.mp4`, `bwi-300-init.mp4` | `samples/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap` (WNUV, 239.255.54.1:5001), packet_id 100 (HEVC 720p) / 200 (AC-4 5.1 eng) / 300 (IMSC1 stpp) | FT=0 DUs, MPU sequence 1676551952/1676551953 |
| `bwi-100-moof.m4s`, `bwi-200-moof.m4s`, `bwi-300-moof.m4s` | same | FT=1 DUs (media traf carries a Smooth Streaming `uuid` tfxd box, tfdt is 0 on every MPU) |

ROUTE initialization segments written by `routedump`, used for the codecs strings:

| file | service | expected codecs |
|---|---|---|
| `route-uni-usc02-video-init.mp4v` | UniSoft bsid 24 service 2 (hev1, 1080p) | `hev1.2.4.L120.B0` (MPD says `.90`, the stream says `B0`) |
| `route-uni-usc02-audio-init.mp4a` | same, mp4a | `mp4a.40.2` |
| `route-bwi-wmar-video-init.mp4` | Sinclair bsid 1409 service 5004 WMAR, hvc1 720p | `hvc1.2.4.L120.B0` |
| `route-bwi-wmar-a0-init.mp4`, `-a1-init.mp4` | WMAR ac-4 5.1 eng / stereo spa | `ac-4.02.01.01` / `ac-4.02.01.00` |
| `route-bwi-wmar-d4-init.mp4` | WMAR stpp captions | `stpp.ttml.im1t` |
| `route-bwi-wbal-video-init.mp4`, `-a0-init.mp4` | service 5003 WBAL, CENC `encv` / `enca` with `sinf/frma` | `hvc1.2.4.L123.B0` / `ac-4.02.01.01` |
