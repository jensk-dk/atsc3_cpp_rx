#pragma once
// STB UI model (spec 5.20.2): the channel list of the current mux, the highlight and tune state, the banner timer and
// the present/following lookup. Free of GStreamer so that the unit suite covers it.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "atsc3/esg/esg.hpp"
#include "atsc3/service/engine.hpp"

namespace atsc3::stb {

/// One row of the channel list: an A/V service of the mux.
struct Channel {
    std::uint16_t bsid = 0, service_id = 0;
    std::optional<std::uint16_t> major, minor;
    std::string name, protocol;                // short name (USBD name, else "service N"); "ROUTE" | "MMTP"
    std::optional<std::string> global_service_id;
    bool protected_flag = false, hidden = false;

    std::string number() const;                // "major.minor", else "#service_id"
    std::string label() const;                 // number + name
    bool sameService(const Channel& o) const noexcept { return bsid == o.bsid && service_id == o.service_id; }
};

/// The channels of the mux from the engine's service list: ROUTE and MMTP services of category 1/2 or with a video
/// or audio component, hidden ones only with `show_hidden`, sorted by channel number then service id.
std::vector<Channel> channelsOf(const std::vector<service::ServiceInfo>& services, bool show_hidden);
/// "major.minor" -> (major, minor); nullopt when the text is not of that form.
std::optional<std::pair<std::uint16_t, std::uint16_t>> parseChannelNumber(const std::string& text);
/// The channel a `--service` argument names: "major.minor" or a service id (any bsid). nullopt when absent.
std::optional<Channel> findChannel(const std::vector<Channel>& channels, const std::string& spec);

struct Programme {
    std::string title;
    std::int64_t start_unix = 0, end_unix = 0;
};
struct NowNext {
    std::optional<Programme> now, next;
};
/// Present/following of a channel at `now_unix`: the ESG Service is matched by globalServiceID, then by major.minor;
/// `now` is the programme whose window contains the instant, `next` the first one starting after it.
NowNext nowNext(const esg::Guide& guide, const Channel& ch, std::int64_t now_unix);

/// Remote-control keys, from GstNavigation key names (keyOf) or the scripted key list.
enum class Key { Up, Down, PageUp, PageDown, Select, Back, List, Info, Quit, None };
Key keyOf(const std::string& name);

struct Action {
    enum class Kind { None, Tune, Quit };
    Kind kind = Kind::None;
    std::optional<Channel> channel;
};

/// Channel list, highlight, tuned channel and banner timer; the key handler returns what the host must do.
///   list hidden:  Up / Down (and PageUp / PageDown) change channel (wrapping), Select / List open the list on the
///                 tuned channel, Info shows the banner, Back hides the banner, Quit quits.
///   list visible: Up / Down move the highlight, PageUp / PageDown by a page, Select tunes the highlighted channel
///                 and closes the list, Back / List close it, Quit quits.
class UiModel {
public:
    double banner_seconds = 5.0;
    int page_rows = 8;

    /// Replaces the list; the tuned and highlighted channels are kept when they are still present.
    void setChannels(std::vector<Channel> channels);
    const std::vector<Channel>& channels() const noexcept { return channels_; }
    std::optional<Channel> tuned() const;
    int tunedIndex() const noexcept { return tuned_; }
    int highlight() const noexcept { return highlight_; }
    bool listVisible() const noexcept { return list_visible_; }
    bool bannerVisible(double now_s) const noexcept { return now_s < banner_until_; }

    /// The host applied a tune: marks the channel and shows the banner.
    void setTuned(const Channel& ch, double now_s);
    void showBanner(double now_s) { banner_until_ = now_s + banner_seconds; }
    void hideBanner() { banner_until_ = 0.0; }
    void showList() { list_visible_ = true; highlight_ = tuned_ >= 0 ? tuned_ : (channels_.empty() ? -1 : 0); }
    void hideList() { list_visible_ = false; }

    Action onKey(Key key, double now_s);

private:
    int indexOf(const Channel& ch) const;
    Action tuneTo(int index, double now_s);

    std::vector<Channel> channels_;
    int tuned_ = -1, highlight_ = -1;
    bool list_visible_ = false;
    double banner_until_ = 0.0;
};

/// A frozen view for the renderer, built by the host once per poll.
struct UiSnapshot {
    bool list_visible = false, banner_visible = false;
    int highlight = -1, tuned = -1;
    std::vector<Channel> channels;
    std::optional<Channel> channel;            // the tuned one
    NowNext epg;
    std::int64_t now_unix = 0;
    std::string status;                        // "Tuning…", "No decodable video", "" while playing
    std::string video_codec, audio_codec;
};

/// "HH:MM" in local time.
std::string clockText(std::int64_t unix_s);

}  // namespace atsc3::stb
