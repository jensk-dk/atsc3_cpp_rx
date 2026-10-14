#!/usr/bin/env python3
"""libvlc-driven scenarios (spec 5.19.5 harness 2) through ctypes, so no python-vlc package is needed.

    libvlc_test.py <plugin-dir> <capture> [service]

Plays atsc3://<capture>?service=... headless, waits for Playing, asserts that the time advances, lists the video,
audio and subtitle tracks, pauses and resumes, then switches program with the --program option on a second player.
Exit 0 pass, 1 fail, 77 skipped (no libvlc / no capture). VLC 3.0 line (libvlc.so.5)."""
import ctypes
import ctypes.util
import os
import sys
import time

if len(sys.argv) < 3:
    print(__doc__)
    sys.exit(2)
plugin_dir, capture = sys.argv[1], sys.argv[2]
service = sys.argv[3] if len(sys.argv) > 3 else ''
if not os.path.isfile(capture):
    print(f'skip: capture {capture} not found')
    sys.exit(77)
os.environ['VLC_PLUGIN_PATH'] = plugin_dir + (':' + os.environ['VLC_PLUGIN_PATH'] if os.environ.get('VLC_PLUGIN_PATH') else '')
libname = os.environ.get('LIBVLC') or ctypes.util.find_library('vlc')
if not libname:
    print('skip: libvlc not found')
    sys.exit(77)
lib = ctypes.CDLL(libname)


class TrackDescription(ctypes.Structure):
    pass


TrackDescription._fields_ = [('i_id', ctypes.c_int), ('psz_name', ctypes.c_char_p), ('p_next', ctypes.POINTER(TrackDescription))]


class MediaStats(ctypes.Structure):
    _fields_ = [('i_read_bytes', ctypes.c_int), ('f_input_bitrate', ctypes.c_float), ('i_demux_read_bytes', ctypes.c_int), ('f_demux_bitrate', ctypes.c_float),
                ('i_demux_corrupted', ctypes.c_int), ('i_demux_discontinuity', ctypes.c_int), ('i_decoded_video', ctypes.c_int), ('i_decoded_audio', ctypes.c_int),
                ('i_displayed_pictures', ctypes.c_int), ('i_lost_pictures', ctypes.c_int), ('i_played_abuffers', ctypes.c_int), ('i_lost_abuffers', ctypes.c_int),
                ('i_sent_packets', ctypes.c_int), ('i_sent_bytes', ctypes.c_int), ('f_send_bitrate', ctypes.c_float)]


def proto(name, restype, *argtypes):
    f = getattr(lib, name)
    f.restype = restype
    f.argtypes = argtypes
    return f


P = ctypes.c_void_p
libvlc_get_version = proto('libvlc_get_version', ctypes.c_char_p)
libvlc_new = proto('libvlc_new', P, ctypes.c_int, ctypes.POINTER(ctypes.c_char_p))
libvlc_release = proto('libvlc_release', None, P)
media_new_location = proto('libvlc_media_new_location', P, P, ctypes.c_char_p)
media_add_option = proto('libvlc_media_add_option', None, P, ctypes.c_char_p)
media_release = proto('libvlc_media_release', None, P)
media_get_stats = proto('libvlc_media_get_stats', ctypes.c_int, P, ctypes.POINTER(MediaStats))
mp_new_from_media = proto('libvlc_media_player_new_from_media', P, P)
mp_play = proto('libvlc_media_player_play', ctypes.c_int, P)
mp_stop = proto('libvlc_media_player_stop', None, P)
mp_release = proto('libvlc_media_player_release', None, P)
mp_get_state = proto('libvlc_media_player_get_state', ctypes.c_int, P)
mp_get_time = proto('libvlc_media_player_get_time', ctypes.c_longlong, P)
mp_set_pause = proto('libvlc_media_player_set_pause', None, P, ctypes.c_int)
video_tracks = proto('libvlc_video_get_track_description', ctypes.POINTER(TrackDescription), P)
audio_tracks = proto('libvlc_audio_get_track_description', ctypes.POINTER(TrackDescription), P)
spu_tracks = proto('libvlc_video_get_spu_description', ctypes.POINTER(TrackDescription), P)
tracks_release = proto('libvlc_track_description_list_release', None, ctypes.POINTER(TrackDescription))
STATE = {0: 'NothingSpecial', 1: 'Opening', 2: 'Buffering', 3: 'Playing', 4: 'Paused', 5: 'Stopped', 6: 'Ended', 7: 'Error'}


def track_names(getter, mp):
    out = []
    p = getter(mp)
    head = p
    while p:
        out.append((p.contents.i_id, (p.contents.psz_name or b'').decode('utf-8', 'replace')))
        p = p.contents.p_next
    if head:
        tracks_release(head)
    return [n for i, n in out if i >= 0]


failures = []
notes = []
args = [b'--vout=dummy', b'--aout=dummy', b'--no-video-title-show', b'--quiet', b'-I', b'dummy']
argv = (ctypes.c_char_p * len(args))(*args)
inst = libvlc_new(len(args), argv)
if not inst:
    print('fail: libvlc_new')
    sys.exit(1)
notes.append('libvlc ' + libvlc_get_version().decode())


def play(mrl, options, seconds, label):
    media = media_new_location(inst, mrl.encode())
    for o in options:
        media_add_option(media, o.encode())
    mp = mp_new_from_media(media)
    mp_play(mp)
    t0 = time.time()
    while time.time() - t0 < 25 and mp_get_state(mp) not in (3, 6, 7):
        time.sleep(0.2)
    state = mp_get_state(mp)
    if state != 3:
        failures.append(f'{label}: state {STATE.get(state, state)} instead of Playing after {time.time() - t0:.1f} s')
        mp_stop(mp)
        mp_release(mp)
        media_release(media)
        return
    notes.append(f'{label}: playing after {time.time() - t0:.1f} s')
    time.sleep(seconds)
    t1 = mp_get_time(mp)
    time.sleep(6)
    t2 = mp_get_time(mp)
    if t2 - t1 < 4000:
        failures.append(f'{label}: time advanced {t2 - t1} ms in 6 s')
    notes.append(f'{label}: time {t1} -> {t2} ms')
    v, a, s = track_names(video_tracks, mp), track_names(audio_tracks, mp), track_names(spu_tracks, mp)
    notes.append(f'{label}: video {v} audio {a} spu {s}')
    if not v:
        failures.append(f'{label}: no video track listed')
    st = MediaStats()
    if media_get_stats(media, ctypes.byref(st)):
        notes.append(f'{label}: decoded video {st.i_decoded_video}, displayed {st.i_displayed_pictures}, lost {st.i_lost_pictures}, audio buffers {st.i_played_abuffers}')
        if st.i_decoded_video <= 0:
            failures.append(f'{label}: no video decoded')
    # pause / resume: the virtual tuner freezes with VLC
    mp_set_pause(mp, 1)
    time.sleep(1.0)
    if mp_get_state(mp) != 4:
        failures.append(f'{label}: not paused ({STATE.get(mp_get_state(mp))})')
    tp = mp_get_time(mp)
    time.sleep(2.0)
    if abs(mp_get_time(mp) - tp) > 500:
        failures.append(f'{label}: time moved while paused')
    mp_set_pause(mp, 0)
    time.sleep(4.0)
    if mp_get_state(mp) != 3:
        failures.append(f'{label}: not playing after resume ({STATE.get(mp_get_state(mp))})')
    tr = mp_get_time(mp)
    time.sleep(4.0)
    if mp_get_time(mp) - tr < 2500:
        failures.append(f'{label}: time did not advance after resume')
    notes.append(f'{label}: pause/resume ok, time {tr} -> {mp_get_time(mp)} ms')
    mp_stop(mp)
    mp_release(mp)
    media_release(media)


mrl = f'atsc3://{capture}' + (f'?service={service}' if service else '')
play(mrl, [], 6, 'first')
# program switch: VLC's --program forces the group; the plugin selects that service at start
alt = os.environ.get('ALT_PROGRAM')
if alt:
    play(f'atsc3://{capture}', [f':program={alt}'], 6, f'program {alt}')
libvlc_release(inst)
print('checks: ' + ' | '.join(notes))
if failures:
    print('FAIL: ' + '; '.join(failures))
    sys.exit(1)
print('PASS')
