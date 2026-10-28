/* This file is part of RawTherapee. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace rtengine {
namespace raw_noise {

struct CameraNoiseSample {
    double iso;
    double a[3]; // R, G, B: variance slope in source reference units
    double b[3]; // variance intercept, NOT standard deviation
};

// Borrowed immutable data; samples must have strictly increasing positive ISO.
// aliases is a comma-separated list of complete model names, as in the TSV.
struct CameraNoiseProfileRecord {
    const char* make;
    const char* model;
    const char* aliases;
    const char* mode;
    bool generic;
    const CameraNoiseSample* samples;
    std::size_t sampleCount;
};

struct CameraNoiseDataInfo {
    unsigned schema;
    const char* inputSHA256;
    std::size_t inputRows;
    std::size_t profileCount;
    std::size_t sampleCount;
};

// A match is only a reference-table lookup. It does NOT verify decoded units,
// black/white levels, sensor mode, or propagation into a working RGB image.
// Never use matched to set a WorkingModel/Model.valid flag without independent
// physical validation. This component has no dependency on raw_noise_model.h.
struct ReferenceCameraProfile {
    bool matched = false;
    std::string reason = "no_match";
    std::string make;
    std::string model;
    std::string aliases;
    std::string mode;
    bool generic = false;
    double iso = 0;
    std::array<double, 4> a{}; // R, G1, B, G2; G2 reuses G1
    std::array<double, 4> b{};
};

// ASCII case/punctuation normalization, known maker spellings, repeated maker
// prefix removal, and Canon-only "Mark II" / "2" folding. Exact make+model
// wins over model-only fallback. Ambiguous matches fail closed. Generic mode
// wins within a single maker/model identity, never across different makers.
ReferenceCameraProfile lookupReferenceCameraProfile(
    const std::string& make, const std::string& model, double iso);

// Same policy with an explicit immutable table, useful for unit tests/clients.
ReferenceCameraProfile lookupReferenceCameraProfile(
    const std::string& make, const std::string& model, double iso,
    const CameraNoiseProfileRecord* records, std::size_t count);

const CameraNoiseDataInfo& cameraNoiseDataInfo();

} // namespace raw_noise
} // namespace rtengine
