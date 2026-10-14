#pragma once
// GStreamer playback of the tuned service (spec 5.20.3). One persistent pipeline keeps the window and the audio
// device open across channel changes:
//   videotestsrc(black) ─┐
//   track branches ──────┴─ compositor ─ videoconvert ─ cairooverlay ─ videoconvert ─ <video sink>
//   audiotestsrc(silence) ┐
//   track branches ───────┴─ audiomixer ─ audioconvert ─ audioresample ─ <audio sink>
// Every decodable video or audio track of the tuned service is a branch "appsrc → decodebin → convert → mixer pad",
// fed with the samples of the fragments the engine delivers (media::fragmentSamples) on one timeline: media time of
// the first fragment is anchored to the pipeline's running time plus a live delay, like the VLC plugin's PCR.
#include <cairo.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/byte_view.hpp"
#include "atsc3/media/isobmff.hpp"

namespace atsc3::stb {

struct PlayerConfig {
    int width = 1280, height = 720;
    std::string video_sink = "autovideosink", audio_sink = "autoaudiosink";
    double live_delay_s = 1.5;
    bool hw_decode = false;            // let decodebin pick hardware (VA-API, V4L2) decoders
    bool verbose = false;
};

struct TrackStats {
    std::string key, kind, codecs;
    std::uint64_t fragments = 0, samples = 0, bytes = 0;
    std::uint64_t decoded = 0;         // buffers that reached the mixer
};

/// Why a track was not added.
struct TrackVerdict {
    bool added = false;
    std::string kind, codecs, caps;    // caps: the GStreamer caps string the track mapped to ("" when unmapped)
    std::string reason;                // "no GStreamer decoder for ...", "encrypted", "subtitles are not rendered", ...
};

class Player {
public:
    using DrawFn = std::function<void(cairo_t*, int width, int height)>;
    using KeyFn = std::function<void(const std::string& key_name)>;
    using MessageFn = std::function<void(const std::string& text, bool fatal)>;

    explicit Player(PlayerConfig cfg);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void setDraw(DrawFn fn);
    void setKey(KeyFn fn);
    void setMessage(MessageFn fn);

    /// Builds the persistent pipeline and sets it PLAYING. False with `error` when an element is missing.
    bool start(std::string* error);
    void stop();

    /// A channel change (or a timeline restart): drops every branch and the timeline anchor. Tracks are added again
    /// as the host sees their init segments.
    void retune();
    bool hasTrack(const std::string& key) const;
    /// Adds a branch for the first non-hint track of an initialization segment. The verdict says what happened.
    TrackVerdict addTrack(const std::string& key, ByteView init);
    /// Feeds the samples of a movie fragment to a track. `first_dts_s` overrides the fragment's tfdt as the media
    /// time of its first sample (MMT: the engine's service timeline); `discontinuity` marks the first buffer.
    void feedFragment(const std::string& key, ByteView fragment, std::optional<double> first_dts_s, bool discontinuity);
    /// True when the timeline had to be re-anchored since the last call (data behind or far ahead of the clock): the
    /// branches were dropped and the host must add the tracks again.
    bool takeRebuild();

    void setLiveDelay(double seconds);
    double liveDelay() const noexcept { return live_delay_s_; }
    std::vector<TrackStats> stats() const;
    /// Video frames that reached the compositor since the last retune().
    std::uint64_t videoFrames() const noexcept { return video_frames_.load(); }
    std::uint64_t audioBuffers() const noexcept { return audio_buffers_.load(); }
    /// Pipeline running time in seconds (0 before start()).
    double runningTime() const;
    /// Writes the pipeline graph (negotiated caps included) as a .dot file when GST_DEBUG_DUMP_DOT_DIR is set.
    void dumpGraph(const char* name) const;

private:
    struct Track;
    GstElement* make(const char* factory, const char* name, std::string* error);
    bool buildPipeline(std::string* error);
    void dropTracks();
    std::int64_t tick(double media_s) const;
    void anchor(double media_s, const char* why);
    static void onPadAdded(GstElement* decodebin, GstPad* pad, gpointer user);
    static int onAutoplugSelect(GstElement* decodebin, GstPad* pad, GstCaps* caps, GstElementFactory* factory, gpointer user);
    static void onDraw(GstElement* overlay, cairo_t* cr, guint64 timestamp, guint64 duration, gpointer user);
    static void onCapsChanged(GstElement* overlay, GstCaps* caps, gpointer user);
    static GstPadProbeReturn onUpstreamEvent(GstPad* pad, GstPadProbeInfo* info, gpointer user);
    static GstPadProbeReturn onMixerBuffer(GstPad* pad, GstPadProbeInfo* info, gpointer user);
    static gboolean onBus(GstBus* bus, GstMessage* msg, gpointer user);
    void message(const std::string& text, bool fatal);

    PlayerConfig cfg_;
    GstElement* pipeline_ = nullptr;
    GstElement* compositor_ = nullptr;
    GstElement* audiomixer_ = nullptr;
    GstElement* overlay_ = nullptr;
    guint bus_watch_ = 0;
    std::map<std::string, std::unique_ptr<Track>> tracks_;
    // timeline
    bool anchored_ = false;
    double anchor_media_s_ = 0.0;
    std::int64_t anchor_running_ns_ = 0;
    double live_delay_s_ = 1.5;
    bool rebuild_ = false;
    std::atomic<std::uint64_t> video_frames_{0}, audio_buffers_{0};
    // overlay
    mutable std::mutex draw_mu_;
    DrawFn draw_;
    int frame_w_ = 0, frame_h_ = 0;
    KeyFn key_;
    MessageFn message_;
};

/// GStreamer caps for a decoder configuration (nullptr when the sample entry is not mapped). Exposed for tests.
GstCaps* capsFor(const media::DecoderConfig& dc);
/// True when an installed decoder accepts the caps; hardware decoders count only with `allow_hardware`.
bool canDecode(GstCaps* caps, bool allow_hardware);

}  // namespace atsc3::stb
