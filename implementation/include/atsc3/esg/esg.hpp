#pragma once
// atsc3::esg — OMA BCAST Service Guide data model with the A/332 extensions and a schedule index (spec 5.17.5,
// Q6). Consumes the `service::EsgFragment`s the service manager extracts from SGDUs: Service, Schedule and
// Content fragments (OMA BCAST SG 1.0.1 §5.1.2 with the ATSC-OMA-SUP schema; A/332 Tables 5.1-5.10) and joins
// them into programmes (service × presentation window × content). A core layer (`src/esg`; spec Q6).
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atsc3/service/esg.hpp"

namespace atsc3::esg {

const char* name() noexcept;

struct LangText {
    std::string lang;   // xml:lang (default "en" per the schemas)
    std::string text;
};

struct Icon {
    std::string uri;    // element text: an NRT Content-Location of the ESG service or an http(s) URL
    std::string mime;   // MIMEType
    std::optional<std::uint32_t> width, height, data_size;
};

/// ContentAdvisoryRatings (A/332 Table 5.3, RRT-based) or OtherRatings (Table 5.4, free-form scheme).
struct Rating {
    std::optional<std::uint8_t> region;                // RegionIdentifier
    std::string description;                           // RatingDescription text ("TV-PG") or RatingInformationString
    std::string lang;
    std::string scheme;                                // OtherRatings@ratingScheme (empty for RRT ratings)
    std::vector<std::pair<std::uint8_t, std::string>> dimensions;   // RatingDimVal: dimension -> value string
};

struct FragmentBase {
    std::string id;
    std::uint32_t version = 0;
    std::optional<std::int64_t> valid_from_unix, valid_to_unix;
    std::string xml;   // the fragment as received
};

/// Service fragment (A/332 Table 5.1).
struct Service : FragmentBase {
    std::optional<std::string> global_service_id;
    std::optional<std::uint32_t> service_type;    // 228 linear, 229 app-based (A/332 §5.2.2.1.1)
    std::optional<std::uint32_t> weight;
    bool emergency = false;
    std::vector<LangText> names, descriptions;
    std::vector<std::string> audio_languages, text_languages;
    std::vector<std::string> genres;              // Genre@href
    std::vector<Rating> ratings;
    std::vector<Icon> icons;                      // ATSC3ServiceExtension/Icon
    std::optional<std::uint16_t> major, minor;    // ATSC3ServiceExtension/MajorChannelNum, MinorChannelNum
};

struct PresentationWindow {
    std::int64_t start_unix = 0, end_unix = 0;
    std::optional<std::uint32_t> duration_s;
    std::optional<std::string> id;
};

struct ContentRef {
    std::string content_id;                 // ContentReference@idRef
    std::vector<PresentationWindow> windows;
};

/// Schedule fragment (A/332 Table 5.6).
struct Schedule : FragmentBase {
    std::string service_id;                 // ServiceReference@idRef
    std::vector<ContentRef> contents;
};

struct Component {
    std::string kind;       // video, audio, cc, app
    std::string language;   // @language
    std::string text;       // element text (codec / role description)
};

/// Content fragment (A/332 Table 5.7) with the content-level private extensions (Table 5.9).
struct Content : FragmentBase {
    std::vector<std::string> service_ids;   // ServiceReference@idRef
    std::vector<LangText> names, descriptions;
    std::optional<double> length_s;         // Length (xs:duration)
    std::vector<std::string> audio_languages, text_languages;
    std::vector<std::string> genres;
    std::vector<Rating> ratings;
    std::vector<Icon> icons;                // ContentIcon
    std::vector<Component> components;      // Components/VideoComponent|AudioComponent|CCComponent|AppComponent
    std::vector<std::string> capabilities;  // Capabilities text (device capabilities, Table 5.10)
};

bool parseService(const std::string& xml, Service& out, std::string* error = nullptr);
bool parseSchedule(const std::string& xml, Schedule& out, std::string* error = nullptr);
bool parseContent(const std::string& xml, Content& out, std::string* error = nullptr);

/// Picks the text in the preferred language, else the first entry; empty when there is none.
std::string pickText(const std::vector<LangText>& texts, std::string_view preferred_lang = "en");

/// One row of the EPG: a presentation window of a Content on a Service.
struct Programme {
    std::string service_id, content_id, schedule_id;
    std::int64_t start_unix = 0, end_unix = 0;
    const Content* content = nullptr;   // nullptr when the Content fragment has not arrived (yet)
};

struct GuideStats {
    std::size_t services = 0, schedules = 0, contents = 0, programmes = 0;
    std::size_t failed = 0, ignored = 0, unresolved_contents = 0;
};

/// Versioned fragment store plus the programme index. Not thread-safe; owners snapshot or lock around it.
class Guide {
public:
    void clear();
    /// Parses and inserts (or replaces, when the version is newer or equal) one fragment. False when it does not
    /// parse; fragments of other types count as ignored.
    bool add(const service::EsgFragment& f, std::string* error = nullptr);
    /// Rebuilds the sorted service list and the programme index; call after a batch of add().
    void build();

    static Guide fromFragments(const std::vector<service::EsgFragment>& fragments);

    const std::vector<const Service*>& services() const noexcept { return services_sorted_; }
    const Service* service(const std::string& id) const;
    const Content* content(const std::string& id) const;
    const Schedule* schedule(const std::string& id) const;
    const std::vector<Programme>& programmes() const noexcept { return programmes_; }
    /// Programmes overlapping [from, to), optionally of one service, sorted by service then start.
    std::vector<Programme> programmes(std::int64_t from_unix, std::int64_t to_unix, const std::string* service_id = nullptr) const;
    /// Earliest start and latest end over all programmes (nullopt when empty).
    std::optional<std::pair<std::int64_t, std::int64_t>> span() const;
    const GuideStats& stats() const noexcept { return stats_; }

private:
    std::map<std::string, Service> services_;
    std::map<std::string, Schedule> schedules_;
    std::map<std::string, Content> contents_;
    std::vector<const Service*> services_sorted_;
    std::vector<Programme> programmes_;
    GuideStats stats_;
};

}  // namespace atsc3::esg
