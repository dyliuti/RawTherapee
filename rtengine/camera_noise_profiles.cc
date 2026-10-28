/* This file is part of RawTherapee. SPDX-License-Identifier: GPL-3.0-or-later */
#include "camera_noise_profiles.h"

#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace rtengine {
namespace raw_noise {
namespace {

#include "camera_noise_profiles.inc"

// Deliberately locale-independent: metadata keys in this table are ASCII.
std::string words(const std::string& text)
{
    std::string key;
    for (unsigned char c : text) {
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            key += static_cast<char>(c);
        } else if (!key.empty() && key.back() != ' ') {
            key += ' ';
        }
    }
    if (!key.empty() && key.back() == ' ') {
        key.pop_back();
    }
    return key;
}

std::string maker(const std::string& text)
{
    const std::string key = words(text);
    static const std::map<std::string, std::string> spellings = {
        {"NIKON CORPORATION", "NIKON"},
        {"OLYMPUS IMAGING CORP", "OLYMPUS"},
        {"OLYMPUS CORPORATION", "OLYMPUS"},
        {"OLYMPUS OPTICAL CO LTD", "OLYMPUS"},
        {"RICOH IMAGING COMPANY LTD", "RICOH"},
        {"PENTAX CORPORATION", "PENTAX"},
        {"LEICA CAMERA AG", "LEICA"},
        {"PHASE ONE A S", "PHASE ONE"}
    };
    const auto found = spellings.find(key);
    return found == spellings.end() ? key : found->second;
}

std::string modelKey(const std::string& make, const std::string& model)
{
    std::string key = words(model);
    const std::string prefix = make + ' ';
    if (!make.empty() && key.compare(0, prefix.size(), prefix) == 0) {
        key.erase(0, prefix.size());
    }
    if (make == "CANON") {
        // "EOS R6 Mark II" and the export's "EOS R6m2" -> EOSR6M2.
        // Do not collapse generations (R6 != R6m2) or apply this to other makes.
        std::istringstream input(key);
        std::vector<std::string> tokens;
        for (std::string token; input >> token;) {
            tokens.push_back(token);
        }
        key.clear();
        static const std::map<std::string, std::string> generations = {
            {"I", "1"}, {"II", "2"}, {"III", "3"}, {"IV", "4"},
            {"V", "5"}, {"VI", "6"}, {"VII", "7"}, {"VIII", "8"},
            {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"},
            {"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"}
        };
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == "MARK" && i + 1 < tokens.size()) {
                const auto generation = generations.find(tokens[i + 1]);
                if (generation != generations.end()) {
                    key += "M" + generation->second;
                    ++i;
                    continue;
                }
            }
            key += tokens[i];
        }
    } else {
        std::string compact;
        for (char c : key) {
            if (c != ' ') {
                compact += c;
            }
        }
        key.swap(compact);
    }
    return key;
}

struct Entry {
    const CameraNoiseProfileRecord* record;
    std::string make;
    std::string identity;
    std::set<std::string> names;
};

std::vector<Entry> makeIndex(const CameraNoiseProfileRecord* records, std::size_t count)
{
    std::vector<Entry> entries;
    for (std::size_t i = 0; records && i < count; ++i) {
        const auto& r = records[i];
        if (!r.make || !r.model || !r.aliases || !r.mode) {
            continue;
        }
        Entry e;
        e.record = &r;
        e.make = maker(r.make);
        e.identity = modelKey(e.make, r.model);
        e.names.insert(e.identity);
        std::istringstream aliases(r.aliases);
        for (std::string alias; std::getline(aliases, alias, ',');) {
            const std::string key = modelKey(e.make, alias);
            if (!key.empty()) {
                e.names.insert(key);
            }
        }
        entries.push_back(e);
    }
    return entries;
}

ReferenceCameraProfile lookup(const std::string& make, const std::string& model,
                              double iso, const std::vector<Entry>& entries)
{
    ReferenceCameraProfile result;
    if (!std::isfinite(iso) || iso <= 0) {
        result.reason = "invalid_iso";
        return result;
    }
    if (words(model).empty()) {
        return result;
    }
    const std::string makeKey = maker(make);
    std::vector<const Entry*> candidates;
    // A make-qualified lookup always precedes the model-only fallback. In
    // fallback, each entry's maker strips an embedded maker from the model.
    for (int pass = 0; pass < 2 && candidates.empty(); ++pass) {
        for (const auto& e : entries) {
            if (pass == 0 && (makeKey.empty() || makeKey != e.make)) {
                continue;
            }
            if (e.names.count(modelKey(e.make, model))) {
                candidates.push_back(&e);
            }
        }
    }
    if (candidates.empty()) {
        return result;
    }
    const Entry& first = *candidates.front();
    for (const Entry* e : candidates) {
        if (e->make != first.make || e->identity != first.identity) {
            result.reason = "ambiguous_profile";
            return result;
        }
    }
    const CameraNoiseProfileRecord* selected = nullptr;
    const bool hasGeneric = [&candidates]() {
        for (const Entry* e : candidates) {
            if (e->record->generic) {
                return true;
            }
        }
        return false;
    }();
    for (const Entry* e : candidates) {
        if (hasGeneric && !e->record->generic) {
            continue;
        }
        // Even identical duplicate records are ambiguous: never first-row wins.
        if (selected) {
            result.reason = "ambiguous_profile";
            return result;
        }
        selected = e->record;
    }
    const auto& r = *selected;
    if (!r.samples || !r.sampleCount) {
        result.reason = "invalid_profile";
        return result;
    }
    for (std::size_t i = 0; i < r.sampleCount; ++i) {
        const auto& s = r.samples[i];
        if (!std::isfinite(s.iso) || s.iso <= 0 || (i && s.iso <= r.samples[i - 1].iso)) {
            result.reason = "invalid_profile";
            return result;
        }
        for (unsigned c = 0; c < 3; ++c) {
            if (!std::isfinite(s.a[c]) || s.a[c] < 0 ||
                !std::isfinite(s.b[c]) || s.b[c] < 0) {
                result.reason = "invalid_profile";
                return result;
            }
        }
    }
    std::size_t hi = 0;
    while (hi < r.sampleCount && r.samples[hi].iso < iso) {
        ++hi;
    }
    const bool exact = hi < r.sampleCount && r.samples[hi].iso == iso;
    const bool outside = hi == 0 || hi == r.sampleCount;
    const auto& upper = r.samples[hi == r.sampleCount ? hi - 1 : hi];
    const auto& lower = r.samples[hi == 0 ? 0 : hi - 1];
    for (unsigned c = 0; c < 3; ++c) {
        if (exact) {
            result.a[c] = upper.a[c];
            result.b[c] = upper.b[c];
        } else if (outside) {
            const double ratio = iso / upper.iso;
            result.a[c] = upper.a[c] * ratio;
            result.b[c] = upper.b[c] * ratio * ratio;
        } else {
            const double t = (iso - lower.iso) / (upper.iso - lower.iso);
            result.a[c] = (1 - t) * lower.a[c] + t * upper.a[c];
            const double read = (1 - t) * std::sqrt(lower.b[c]) + t * std::sqrt(upper.b[c]);
            result.b[c] = read * read;
        }
        if (!std::isfinite(result.a[c]) || !std::isfinite(result.b[c])) {
            // Do not leak partially evaluated coefficients on failure.
            result = ReferenceCameraProfile();
            result.reason = "coefficient_overflow";
            return result;
        }
    }
    result.a[3] = result.a[1];
    result.b[3] = result.b[1];
    result.make = r.make;
    result.model = r.model;
    result.aliases = r.aliases;
    result.mode = r.mode;
    result.generic = r.generic;
    result.iso = iso;
    result.matched = true;
    result.reason.clear();
    return result;
}

} // namespace

ReferenceCameraProfile lookupReferenceCameraProfile(
    const std::string& make, const std::string& model, double iso)
{
    // C++11 thread-safe initialization; no filesystem or TSV access here.
    static const std::vector<Entry> entries = makeIndex(
        kCameraNoiseProfiles, sizeof(kCameraNoiseProfiles) / sizeof(kCameraNoiseProfiles[0]));
    return lookup(make, model, iso, entries);
}

ReferenceCameraProfile lookupReferenceCameraProfile(
    const std::string& make, const std::string& model, double iso,
    const CameraNoiseProfileRecord* records, std::size_t count)
{
    return lookup(make, model, iso, makeIndex(records, count));
}

const CameraNoiseDataInfo& cameraNoiseDataInfo()
{
    return kCameraNoiseDataInfo;
}

} // namespace raw_noise
} // namespace rtengine
