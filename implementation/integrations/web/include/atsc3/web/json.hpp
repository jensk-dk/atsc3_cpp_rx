#pragma once
// Minimal JSON writer (no dependency): objects, arrays, strings with escaping, numbers, bools, null.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atsc3::web {

class Json {
public:
    Json& beginObject();
    Json& endObject();
    Json& beginArray();
    Json& endArray();
    Json& key(const std::string& k);
    Json& value(const std::string& s);
    Json& value(const char* s);
    Json& value(bool b);
    Json& value(double d);
    Json& value(std::int64_t v);
    Json& value(std::uint64_t v);
    Json& value(int v) { return value(static_cast<std::int64_t>(v)); }
    Json& value(unsigned v) { return value(static_cast<std::uint64_t>(v)); }
    Json& null();
    Json& raw(const std::string& json);  // pre-encoded
    template <class T> Json& value(const std::optional<T>& v) { return v ? value(*v) : null(); }
    template <class T> Json& kv(const std::string& k, const T& v) { return key(k).value(v); }
    Json& kv(const std::string& k, const char* v) { return key(k).value(v); }
    Json& kv(const std::string& k, const std::string& v) { return key(k).value(v); }
    Json& kv(const std::string& k, bool v) { return key(k).value(v); }
    template <class T> Json& kvList(const std::string& k, const std::vector<T>& items) {
        key(k).beginArray();
        for (const T& i : items) value(i);
        return endArray();
    }
    const std::string& str() const noexcept { return out_; }
    static std::string escape(const std::string& s);

private:
    void sep();
    std::string out_;
    std::vector<bool> first_;  // per nesting level: nothing written yet
    bool after_key_ = false;
};

}  // namespace atsc3::web
