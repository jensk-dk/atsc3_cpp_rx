#include "atsc3/service/tuner.hpp"

namespace atsc3::service {

const char* toString(TunerOutput o) noexcept {
    switch (o) {
        case TunerOutput::Bbp: return "bbp";
        case TunerOutput::Alp: return "alp";
        case TunerOutput::Ip: return "ip";
    }
    return "?";
}

const char* toString(TunerError e) noexcept {
    switch (e) {
        case TunerError::None: return "none";
        case TunerError::NoAtscContent: return "no ATSC 3.0 content";
        case TunerError::FileNotFound: return "file not found";
        case TunerError::BadFile: return "bad file";
        case TunerError::AlreadyStarted: return "already started";
        case TunerError::NotOpen: return "not open";
        case TunerError::UnsupportedOutput: return "unsupported output";
        case TunerError::Driver: return "driver error";
    }
    return "?";
}

}  // namespace atsc3::service
