/*
 * This file is part of RawTherapee.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "camera_noise_profiles.h"

namespace rtengine {
namespace raw_noise {

// The user-provided camera_model.tsv is authorized for offline compilation
// into camera_noise_profiles.inc. Its reference A/B coefficients are useful
// without a verified readout mode or decoded-DN physical calibration. Keep
// that reference evidence separate from the optional strict sensor contract
// below and from noise propagation into the actual working image.

// Channels are R, G1, B, G2 (not RGGB spatial order). Coefficients describe
// independent, pre-demosaic sensor samples ONLY:
//   x = (decoded_sample - blackDN[c]) / (whiteDN[c] - blackDN[c])
//   Var(x) = shot[c] * x + read[c], for unclipped x in [0, 1].
// No WB, exposure, baseline exposure, lens gain, resampling or colour transform.
// DN means RawImage decoded sample units, not necessarily native ADC codes.
struct SensorCalibration {
    std::string make;
    std::string model;
    // Must identify the measured bit depth/readout/conversion-gain mode.
    // ISO alone cannot identify dual gain or reduced/compressed RAW modes.
    std::string mode;
    double iso = 0;
    std::array<double, 4> blackDN{};
    std::array<double, 4> whiteDN{};
    std::array<double, 4> shot{};
    std::array<double, 4> read{}; // variance intercept, NOT read-noise stddev
    std::string provenance;
    std::string license;
    // These are assertions by the calibration provider after review, not
    // inferred from a nonempty license string or from plausible coefficients.
    bool redistributionReviewed = false;
    bool decodedUnitsVerified = false;
};

struct SensorLookup {
    bool valid = false;
    std::string reason = "no_authorized_calibration";
    SensorCalibration calibration;
};

// Strict physical calibration only: no model-only fallback, fuzzy aliases,
// ISO interpolation/extrapolation, or generic mode selection here. Reference
// lookup uses lookupReferenceCameraProfile independently of these checks.
// The optional records are physical calibrations, NOT the compiled TSV table.
inline SensorLookup lookupSensorNoise(const std::string& make,
                                      const std::string& model,
                                      const std::string& mode,
                                      double iso,
                                      const std::vector<SensorCalibration>& records = {})
{
    SensorLookup result;
    if (make.empty() || model.empty() || mode.empty() || !std::isfinite(iso) || iso <= 0) {
        result.reason = "camera_mode_or_iso_unknown";
        return result;
    }
    const SensorCalibration* match = nullptr;
    for (const auto& record : records) {
        if (record.make == make && record.model == model && record.mode == mode && record.iso == iso) {
            if (match) {
                result.reason = "ambiguous_calibration";
                return result;
            }
            match = &record;
        }
    }
    if (!match) {
        return result;
    }
    if (!match->redistributionReviewed || match->license.empty() || match->provenance.empty()) {
        result.reason = "calibration_license_unverified";
        return result;
    }
    if (!match->decodedUnitsVerified) {
        result.reason = "calibration_units_unverified";
        return result;
    }
    for (unsigned c = 0; c < 4; ++c) {
        if (!std::isfinite(match->blackDN[c]) || !std::isfinite(match->whiteDN[c]) ||
            match->whiteDN[c] <= match->blackDN[c] ||
            !std::isfinite(match->whiteDN[c] - match->blackDN[c]) ||
            !std::isfinite(match->shot[c]) || match->shot[c] < 0 ||
            !std::isfinite(match->read[c]) || match->read[c] < 0 ||
            (match->shot[c] == 0 && match->read[c] == 0)) {
            result.reason = "invalid_calibration_coefficients_or_levels";
            return result;
        }
    }
    result.calibration = *match;
    result.valid = true;
    result.reason.clear();
    return result;
}

// Evaluate a SENSOR sample, not a demosaiced/working RGB pixel. For
// D = whiteDN-blackDN, Var(DN) = shot*D*(sampleDN-blackDN) + read*D^2.
// Return false at/outside clipping boundaries instead of modelling censored
// distributions with an unbounded Poisson-Gaussian variance.
inline bool sensorVarianceDN(const SensorLookup& lookup, unsigned channel,
                             double sampleDN, double& varianceDN)
{
    varianceDN = 0;
    if (!lookup.valid || channel >= 4 || !std::isfinite(sampleDN)) {
        return false;
    }
    const auto& c = lookup.calibration;
    const double span = c.whiteDN[channel] - c.blackDN[channel];
    if (!std::isfinite(span) || span <= 0 || sampleDN <= c.blackDN[channel] ||
        sampleDN >= c.whiteDN[channel] || !std::isfinite(c.shot[channel]) ||
        c.shot[channel] < 0 || !std::isfinite(c.read[channel]) || c.read[channel] < 0) {
        return false;
    }
    const double value = c.shot[channel] * span * (sampleDN - c.blackDN[channel]) +
        c.read[channel] * span * span;
    if (!std::isfinite(value) || value < 0) {
        return false;
    }
    varianceDN = value;
    return true;
}

// Quality of the available SENSOR evidence, not validity in working RGB.
enum class ProfileQuality {
    Estimated,  // unprofiled; adaptive algorithms must estimate their noise
    Reference,  // table match; decoded units/mode are not physically verified
    Calibrated  // strict SensorLookup; working propagation still unverified
};

// Internal snapshot for a working-session owner. This is not a public engine
// type. Copy it while the source is alive, with the same external serialization
// used for load/preprocess/getImage/convertColorSpace. No borrowed pointers.
struct Model {
    bool valid = false; // calibrated noise in the ACTUAL working image
    std::string reason = "source_not_rendered";
    std::string fallback = "use_uncalibrated_denoise";
    std::string make;
    std::string model;
    double iso = 0;
    SensorLookup sensor;

    ProfileQuality profileQuality = ProfileQuality::Estimated;
    // Reference A/B in profile.a/profile.b, ordered R, G1, B, G2. Preserve
    // lookup identity/mode and ISO diagnostics even when working valid=false.
    // Never reinterpret these coefficients as SensorCalibration.shot/read.
    ReferenceCameraProfile profile;
    double rawCodeScale = 65535.0; // nominal engine code scale, NOT ADC white DN
    std::string profileReason = "unprofiled"; // independent of render/color reason

    // Diagnostic metadata only, not proof of a calibrated sensor->RGB map.
    bool renderMetadataValid = false;
    std::array<double, 4> decoderBlackDN{};
    std::array<double, 4> decoderWhiteDN{};
    std::array<double, 4> preprocessingBlackDN{};
    std::array<double, 4> preprocessingWhiteDN{}; // exposure-adjusted white
    std::array<double, 4> preprocessingScale{}; // engine units per decoded DN
    // Actual getImage channel factors INCLUDING WB, gain, baseline exposure
    // and division by skip^2. Do not apply those gains a second time.
    std::array<double, 3> renderMultiplier{};
    double baselineExposureEV = 0;
    int renderSkip = 0;

    // Row-major matrix: workingRGB = matrix * getImageRGB. BOTH sides use
    // engine units (nominal white 65535), NOT unit-range RGB. Does not include
    // preprocessingScale or renderMultiplier. True only for the observed
    // direct matrix conversion, never an approximation to DCP/ICC.
    bool colorMatrixValid = false;
    std::string colorReason = "conversion_not_observed";
    std::string workingProfile;
    std::array<std::array<double, 3>, 3> cameraToWorking{};
};

// Minimal engineering convention, NOT a physical CFA/demosaic covariance.
// x and y use nominal unit range (working pixels / 65535). Var(x)=A*x+B.
// y ~= cameraToWorking*x; stage3Gain MUST stay 1: WB/exposure are in the map.
struct ReferenceWorkingNoise {
    bool referenceUsable = false;
    std::string reason = "source_not_rendered";
    std::array<double, 4> normalizedA{}, normalizedB{}, gain{};
    std::array<std::array<double, 3>, 3> cameraToWorking{};
    double stage3Gain = 1;
};

inline bool referenceMatrixUsable(const std::array<std::array<double, 3>, 3>& m)
{
    double scale = 0;
    for (const auto& row : m) for (double v : row) {
        if (!std::isfinite(v)) return false;
        scale = std::max(scale, std::abs(v));
    }
    if (!(scale > 0)) return false;
    auto a = m;
    for (auto& row : a) for (double& v : row) v /= scale;
    const double det = a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
        - a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
        + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    return std::isfinite(det) && std::abs(det) > 1e-12;
}

inline ReferenceWorkingNoise referenceWorkingNoise(const Model& m,
                                                  bool spatiallyResampled = false)
{
    ReferenceWorkingNoise r;
    if (!m.profile.matched) { r.reason = "reference_profile_unmatched"; return r; }
    // Preserve normalized reference evidence even if the render/map is unusable.
    for (unsigned c = 0; c < 4; ++c) {
        const double a = m.profile.a[c], b = m.profile.b[c];
        if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || b < 0 || (a == 0 && b == 0)) {
            r.reason = "invalid_reference_coefficients"; return r;
        }
        r.normalizedA[c] = a / 65535.0;
        r.normalizedB[c] = b / (65535.0 * 65535.0);
    }
    if (spatiallyResampled || m.renderSkip != 1) {
        r.reason = "spatial_resampling_not_modelled"; return r;
    }
    if (!m.renderMetadataValid) { r.reason = "render_scaling_not_observed_or_invalid"; return r; }
    for (unsigned c = 0; c < 4; ++c) {
        const double span = m.decoderWhiteDN[c] - m.decoderBlackDN[c];
        const double p = m.preprocessingScale[c];
        const double g = m.renderMultiplier[c == 3 ? 1 : c];
        if (!std::isfinite(m.decoderWhiteDN[c]) || !std::isfinite(m.decoderBlackDN[c]) ||
            !std::isfinite(span) || span <= 0 || !std::isfinite(p) || p <= 0 ||
            !std::isfinite(g) || g <= 0) {
            r.reason = "invalid_decoder_levels_or_gain"; return r;
        }
        r.gain[c] = p * g * span / 65535.0;
        if (!std::isfinite(r.gain[c]) || r.gain[c] <= 0) {
            r.reason = "invalid_decoder_levels_or_gain"; return r;
        }
    }
    if (!m.colorMatrixValid) { r.reason = m.colorReason; return r; }
    if (!referenceMatrixUsable(m.cameraToWorking)) {
        r.reason = "nonfinite_or_singular_color_matrix"; return r;
    }
    const double gains[3] = {r.gain[0], r.gain[1]*0.5 + r.gain[3]*0.5, r.gain[2]};
    auto map = m.cameraToWorking;
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = 0; j < 3; ++j) map[i][j] *= gains[j];
    if (!referenceMatrixUsable(map)) { r.reason = "nonfinite_or_singular_reference_map"; return r; }
    r.cameraToWorking = map;
    r.referenceUsable = true;
    r.reason = "reference_approximation_not_exact_cfa_covariance";
    return r;
}

// Explicit fields, no padding/pointers. FNV-1a is a diagnostic snapshot identity,
// not a cryptographic digest. Include full (untruncated) profile identity.
inline uint64_t referenceNoiseHash(const Model& m, const ReferenceWorkingNoise& r,
                                  const std::array<uint32_t, 5>& sizeAndStage, double scale)
{
    uint64_t h = UINT64_C(14695981039346656037);
    auto bytes = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= UINT64_C(1099511628211); }
    };
    auto text = [&](const std::string& s) { bytes(s.c_str(), s.size()+1); };
    auto number = [&](double v) { // fixed little-endian IEEE representation
        uint64_t bits; std::memcpy(&bits, &v, sizeof(bits));
        for (unsigned i = 0; i < 8; ++i) { const unsigned char b = (bits >> (8*i)) & 255; bytes(&b, 1); }
    };
    for (const auto* s : {&m.make, &m.model, &m.profile.make, &m.profile.model,
                         &m.profile.aliases, &m.profile.mode, &m.profile.reason,
                         &m.workingProfile, &m.colorReason, &r.reason}) text(*s);
    for (double v : {m.iso, m.profile.iso, m.rawCodeScale, m.baselineExposureEV,
                     double(m.renderSkip), double(m.profile.generic), double(m.profile.matched),
                     double(m.renderMetadataValid), double(m.colorMatrixValid), double(r.referenceUsable),
                     r.stage3Gain, scale}) number(v);
    for (uint32_t v : sizeAndStage) number(v);
    for (const auto* a : {&m.profile.a, &m.profile.b, &m.decoderBlackDN, &m.decoderWhiteDN,
                         &m.preprocessingBlackDN, &m.preprocessingWhiteDN, &m.preprocessingScale,
                         &r.normalizedA, &r.normalizedB, &r.gain}) for (double v : *a) number(v);
    for (double v : m.renderMultiplier) number(v);
    for (const auto* a : {&m.cameraToWorking, &r.cameraToWorking})
        for (const auto& row : *a) for (double v : row) number(v);
    return h;
}

// Attach independently obtained evidence. Reference lookup does not need the
// physical provider's license/unit assertions or an exact measured mode.
// Replacing evidence cannot certify the demosaic/DCP/ICC/working noise path.
inline void setNoiseProfiles(Model& model, const ReferenceCameraProfile& reference,
                             const SensorLookup& sensor = {})
{
    model.profile = reference;
    model.sensor = sensor;
    model.valid = false;
    if (sensor.valid) {
        model.profileQuality = ProfileQuality::Calibrated;
        model.profileReason = "sensor_calibrated_working_propagation_unverified";
        model.fallback = "use_sensor_calibration_with_unverified_working_propagation";
    } else if (reference.matched) {
        model.profileQuality = ProfileQuality::Reference;
        model.profileReason = "reference_profile_physical_units_and_working_propagation_unverified";
        model.fallback = "use_reference_profile_for_adaptive_estimation";
    } else {
        model.profileQuality = ProfileQuality::Estimated;
        model.profileReason = "unprofiled:" + reference.reason;
        model.fallback = "use_uncalibrated_denoise";
    }
}

} // namespace raw_noise
} // namespace rtengine
