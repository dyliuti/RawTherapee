/*
 * This file is part of RawTherapee.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../rtengine/raw_noise_model.h"
// Standalone: c++ -std=c++11 tests/raw_noise_model_test.cpp
//            rtengine/camera_noise_profiles.cc -o /tmp/raw_noise_model_test

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace noise = rtengine::raw_noise;
unsigned checks = 0;

// Unlike assert(), these checks remain active with NDEBUG.
void check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

noise::SensorCalibration fixture()
{
    // Entirely invented arithmetic fixture, NOT a real camera calibration.
    noise::SensorCalibration c;
    c.make = "SYNTHETIC_TEST_ONLY";
    c.model = "invented-four-channel-sensor";
    c.mode = "invented-decoded-DN-mode";
    c.iso = 100;
    c.blackDN = {{16, 32, 64, 128}};
    c.whiteDN = {{144, 288, 576, 1152}};
    c.shot = {{1.0 / 128, 1.0 / 64, 1.0 / 32, 1.0 / 16}};
    c.read = {{1.0 / 16384, 4.0 / 65536, 9.0 / 262144, 16.0 / 1048576}};
    c.provenance = "Invented in this test; no measurement or imported dataset";
    c.license = "GPL-3.0-or-later (synthetic test values only)";
    c.redistributionReviewed = true;
    c.decodedUnitsVerified = true; // assertion for the invented unit convention only
    return c;
}

noise::SensorLookup lookup(const noise::SensorCalibration& c)
{
    return noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso, {c});
}

void rejected(const noise::SensorLookup& result, const char* reason)
{
    CHECK(!result.valid);
    CHECK(result.reason == reason);
    double variance = -123;
    CHECK(!noise::sensorVarianceDN(result, 0, 80, variance));
    CHECK(variance == 0);
}

void badCoefficients(const noise::SensorCalibration& c)
{
    rejected(lookup(c), "invalid_calibration_coefficients_or_levels");
}

void defaults()
{
    const noise::Model model;
    CHECK(!model.valid);
    CHECK(model.reason == "source_not_rendered");
    CHECK(model.fallback == "use_uncalibrated_denoise");
    CHECK(!model.sensor.valid);
    CHECK(model.sensor.reason == "no_authorized_calibration");
    CHECK(!model.renderMetadataValid);
    CHECK(!model.colorMatrixValid);
    CHECK(model.colorReason == "conversion_not_observed");
    CHECK(model.iso == 0);
    CHECK(model.renderSkip == 0);
    CHECK(model.profileQuality == noise::ProfileQuality::Estimated);
    CHECK(!model.profile.matched);
    CHECK(model.profileReason == "unprofiled");
    CHECK(model.rawCodeScale == 65535.0);
    rejected(noise::SensorLookup{}, "no_authorized_calibration");
    const auto c = fixture();
    rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso),
             "no_authorized_calibration");
    rejected(lookup(noise::SensorCalibration{}), "camera_mode_or_iso_unknown");
    noise::Model sensorOnly;
    sensorOnly.sensor = lookup(c);
    CHECK(sensorOnly.sensor.valid);
    CHECK(!sensorOnly.valid); // Sensor calibration is NOT a working-RGB model.
}

void referenceVsCalibrated()
{
    // Mock reference table: no measured readout mode or physical assertions.
    const auto c = fixture();
    const noise::CameraNoiseSample samples[] = {
        {100, {1, 2, 3}, {4, 9, 16}},
        {200, {3, 4, 5}, {16, 25, 36}}
    };
    const noise::CameraNoiseProfileRecord row = {
        "SYNTHETIC_TEST_ONLY", "invented-four-channel-sensor", "", "", true,
        samples, 2
    };
    noise::Model model;
    const auto noProfile = noise::lookupReferenceCameraProfile(
        "unknown", "unprofiled", 100, &row, 1);
    noise::setNoiseProfiles(model, noProfile);
    CHECK(!model.valid);
    CHECK(!model.profile.matched);
    CHECK(!model.sensor.valid);
    CHECK(model.profileQuality == noise::ProfileQuality::Estimated);
    CHECK(model.profileReason == "unprofiled:no_match");
    CHECK(model.fallback == "use_uncalibrated_denoise");

    // Reference remains available before rendering, and across nonlinear color
    // paths. These observations must not be changed by profile attachment.
    for (const char* colorReason : {"conversion_not_observed",
             "dcp_transform_not_certified_linear", "icc_transform_not_certified_linear", ""}) {
        model.colorReason = colorReason;
        model.colorMatrixValid = model.colorReason.empty();
        model.renderMetadataValid = model.colorMatrixValid;
        model.renderMultiplier = {{2, 3, 4}};
        model.cameraToWorking[0] = {{1, 2, 3}};
        const auto observations = model;
        for (double iso : {50.0, 100.0, 150.0, 400.0}) {
            const auto reference = noise::lookupReferenceCameraProfile(
                c.make, c.model, iso, &row, 1);
            CHECK(reference.matched);
            CHECK(reference.generic);
            CHECK(reference.mode.empty());
            // Missing physical mode rejects only the optional strict model.
            const auto noMode = noise::lookupSensorNoise(c.make, c.model, "", iso, {c});
            CHECK(!noMode.valid);
            noise::setNoiseProfiles(model, reference, noMode);
            CHECK(model.profileQuality == noise::ProfileQuality::Reference);
            CHECK(model.profile.matched);
            CHECK(model.profile.iso == iso);
            CHECK(model.profile.a == reference.a);
            CHECK(model.profile.b == reference.b);
            CHECK(model.profile.a[3] == model.profile.a[1]);
            CHECK(model.profile.b[3] == model.profile.b[1]);
            CHECK(model.rawCodeScale == 65535.0);
            CHECK(!model.sensor.valid);
            CHECK(!model.valid); // Even observed WB + direct matrix is not proof.
            CHECK(model.fallback == "use_reference_profile_for_adaptive_estimation");
            CHECK(model.profileReason ==
                "reference_profile_physical_units_and_working_propagation_unverified");
            CHECK(model.colorReason == observations.colorReason);
            CHECK(model.colorMatrixValid == observations.colorMatrixValid);
            CHECK(model.renderMetadataValid == observations.renderMetadataValid);
            CHECK(model.renderMultiplier == observations.renderMultiplier);
            CHECK(model.cameraToWorking == observations.cameraToWorking);
            CHECK(model.reason == observations.reason);
            // Exercise exact, interpolation and both extrapolation directions.
            const double expectedA = iso == 50 ? .5 : iso == 100 ? 1 : iso == 150 ? 2 : 6;
            const double expectedB = iso == 50 ? 1 : iso == 100 ? 4 : iso == 150 ? 9 : 64;
            CHECK(model.profile.a[0] == expectedA);
            CHECK(model.profile.b[0] == expectedB);
        }
    }

    const auto reference = noise::lookupReferenceCameraProfile(c.make, c.model, 100, &row, 1);
    auto unverified = c;
    unverified.redistributionReviewed = false;
    unverified.decodedUnitsVerified = false;
    noise::setNoiseProfiles(model, reference, lookup(unverified));
    CHECK(model.profileQuality == noise::ProfileQuality::Reference);
    CHECK(model.profile.matched && !model.sensor.valid && !model.valid);

    noise::setNoiseProfiles(model, reference, lookup(c));
    CHECK(model.profileQuality == noise::ProfileQuality::Calibrated);
    CHECK(model.profile.matched);
    CHECK(model.sensor.valid);
    CHECK(!model.valid); // Physical sensor evidence still is not working noise.
    CHECK(model.profileReason == "sensor_calibrated_working_propagation_unverified");
    double variance = 0;
    CHECK(noise::sensorVarianceDN(model.sensor, 0, 80, variance));
    CHECK(variance == 65);
    // Physical calibration does not require a reference table row either.
    noise::setNoiseProfiles(model, noProfile, lookup(c));
    CHECK(model.profileQuality == noise::ProfileQuality::Calibrated);
    CHECK(!model.profile.matched && model.sensor.valid && !model.valid);
    // Reuse must clear calibrated/reference evidence on an unknown camera.
    noise::setNoiseProfiles(model, noProfile);
    CHECK(model.profileQuality == noise::ProfileQuality::Estimated);
    CHECK(!model.sensor.valid && !model.profile.matched && !model.valid);
    CHECK(model.profile.a == (std::array<double, 4>{}));
    CHECK(model.profile.b == (std::array<double, 4>{}));
}

void strictLookup()
{
    const auto c = fixture();
    const auto good = lookup(c);
    CHECK(good.valid);
    CHECK(good.reason.empty());
    CHECK(good.calibration.make == c.make);
    CHECK(good.calibration.model == c.model);
    CHECK(good.calibration.mode == c.mode);
    CHECK(good.calibration.iso == c.iso);
    CHECK(good.calibration.blackDN == c.blackDN);
    CHECK(good.calibration.whiteDN == c.whiteDN);
    CHECK(good.calibration.shot == c.shot);
    CHECK(good.calibration.read == c.read);
    CHECK(good.calibration.provenance == c.provenance);
    CHECK(good.calibration.license == c.license);
    CHECK(good.calibration.redistributionReviewed);
    CHECK(good.calibration.decodedUnitsVerified);

    for (unsigned field = 0; field < 4; ++field) {
        auto bad = c;
        if (field == 0) bad.redistributionReviewed = false;
        if (field == 1) bad.license.clear();
        if (field == 2) bad.provenance.clear();
        if (field == 3) bad.decodedUnitsVerified = false;
        rejected(lookup(bad), field == 3 ? "calibration_units_unverified"
                                         : "calibration_license_unverified");
        // Even an unusable duplicate must not be silently ignored.
        rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso, {c, bad}),
                 "ambiguous_calibration");
        rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso, {bad, c}),
                 "ambiguous_calibration");
    }
    rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso, {c, c}),
             "ambiguous_calibration");
    auto invalidDuplicate = c;
    invalidDuplicate.shot[3] = -1;
    rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso, {c, invalidDuplicate}),
             "ambiguous_calibration");

    // Exact matching only: no aliases, whitespace normalization or mode fallback.
    const std::string alternatives[] = {"", "unknown", "synthetic_test_only", c.make + " "};
    for (const auto& make : alternatives) {
        rejected(noise::lookupSensorNoise(make, c.model, c.mode, c.iso, {c}),
                 make.empty() ? "camera_mode_or_iso_unknown" : "no_authorized_calibration");
    }
    for (const auto& model : {std::string(), std::string("unknown"), c.model + " "}) {
        rejected(noise::lookupSensorNoise(c.make, model, c.mode, c.iso, {c}),
                 model.empty() ? "camera_mode_or_iso_unknown" : "no_authorized_calibration");
    }
    for (const auto& mode : {std::string(), std::string("unknown"), c.mode + " "}) {
        rejected(noise::lookupSensorNoise(c.make, c.model, mode, c.iso, {c}),
                 mode.empty() ? "camera_mode_or_iso_unknown" : "no_authorized_calibration");
    }
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (double iso : {0.0, -0.0, -1.0, inf, -inf, nan}) {
        rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, iso, {c}),
                 "camera_mode_or_iso_unknown");
    }
    const auto low = c;
    auto high = c;
    high.iso = 400;
    for (double iso : {50.0, 200.0, 800.0, std::nextafter(100.0, 0.0),
                       std::nextafter(100.0, inf)}) {
        rejected(noise::lookupSensorNoise(c.make, c.model, c.mode, iso, {low, high}),
                 "no_authorized_calibration");
    }
    CHECK(noise::lookupSensorNoise(c.make, c.model, c.mode, c.iso, {high, c}).valid);
    for (double iso : {std::numeric_limits<double>::min(),
                       std::numeric_limits<double>::max()}) {
        auto finite = c;
        finite.iso = iso;
        CHECK(lookup(finite).valid); // Finite positive exact ISO, no arbitrary cap.
    }
}

void coefficientBoundaries()
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double max = std::numeric_limits<double>::max();
    for (unsigned channel = 0; channel < 4; ++channel) {
        for (double value : {inf, -inf, nan}) {
            for (unsigned field = 0; field < 4; ++field) {
                auto c = fixture();
                if (field == 0) c.blackDN[channel] = value;
                if (field == 1) c.whiteDN[channel] = value;
                if (field == 2) c.shot[channel] = value;
                if (field == 3) c.read[channel] = value;
                badCoefficients(c);
            }
        }
        auto c = fixture();
        c.whiteDN[channel] = c.blackDN[channel];
        badCoefficients(c);
        c.whiteDN[channel] = c.blackDN[channel] - 1;
        badCoefficients(c);
        c.blackDN[channel] = -max;
        c.whiteDN[channel] = max; // Both finite, but their difference overflows.
        badCoefficients(c);
        c = fixture();
        c.shot[channel] = -1;
        badCoefficients(c);
        c = fixture();
        c.read[channel] = -1;
        badCoefficients(c);
        c = fixture();
        c.shot[channel] = 0;
        c.read[channel] = 0;
        badCoefficients(c);
        c = fixture();
        c.shot[channel] = 0;
        CHECK(lookup(c).valid); // Read-only and shot-only models are allowed.
        c = fixture();
        c.read[channel] = 0;
        CHECK(lookup(c).valid);
    }
}

void varianceRejected(const noise::SensorLookup& result, unsigned channel, double sample)
{
    double variance = -123; // Rejection must actively clear the output.
    CHECK(!noise::sensorVarianceDN(result, channel, sample, variance));
    CHECK(variance == 0);
}

void varianceUnitsAndBoundaries()
{
    const auto c = fixture();
    const auto good = lookup(c);
    const double slopeDN[] = {1, 4, 16, 64}; // R, G1, B, G2, not spatial RGGB.
    const double interceptDN2[] = {1, 4, 9, 16}; // Variance, NOT stddev.
    const double max = std::numeric_limits<double>::max();
    const double inf = std::numeric_limits<double>::infinity();
    for (unsigned channel = 0; channel < 4; ++channel) {
        const double black = c.blackDN[channel];
        const double white = c.whiteDN[channel];
        const double span = white - black;
        for (double fraction : {0.25, 0.5, 0.75}) {
            const double sample = black + fraction * span;
            const double expected = slopeDN[channel] * (sample - black) + interceptDN2[channel];
            double variance = -123;
            CHECK(noise::sensorVarianceDN(good, channel, sample, variance));
            CHECK(variance == expected); // Binary-exact fixture, no tolerance needed.
            CHECK(variance / (span * span) == c.shot[channel] * fraction + c.read[channel]);

            auto scaled = c;
            for (unsigned i = 0; i < 4; ++i) {
                scaled.blackDN[i] *= 2;
                scaled.whiteDN[i] *= 2;
            }
            CHECK(noise::sensorVarianceDN(lookup(scaled), channel, sample * 2, variance));
            CHECK(variance == expected * 4); // DN doubles => variance in DN^2 quadruples.
        }
        for (double sample : {black, white, std::nextafter(black, -inf),
                              std::nextafter(white, inf), -max, max, inf, -inf,
                              std::numeric_limits<double>::quiet_NaN()}) {
            varianceRejected(good, channel, sample);
        }
        for (double sample : {std::nextafter(black, white), std::nextafter(white, black)}) {
            double variance = -123;
            CHECK(noise::sensorVarianceDN(good, channel, sample, variance));
            CHECK(std::isfinite(variance));
            CHECK(variance > 0);
        }
        // Lookup accepts finite coefficients; evaluation must still reject overflow.
        for (bool shotOverflow : {false, true}) {
            auto huge = c;
            if (shotOverflow) huge.shot[channel] = max;
            else huge.read[channel] = max;
            const auto hugeLookup = lookup(huge);
            CHECK(hugeLookup.valid);
            varianceRejected(hugeLookup, channel, black + span / 2);
        }
    }
    varianceRejected(good, 4, 80);
    varianceRejected(good, std::numeric_limits<unsigned>::max(), 80);
    varianceRejected(noise::SensorLookup{}, 0, 80);
}
} // namespace

int main()
{
    defaults();
    referenceVsCalibrated();
    strictLookup();
    coefficientBoundaries();
    varianceUnitsAndBoundaries();
    std::cout << "PASS raw_noise_model_test (" << checks << " checks; synthetic fixtures only)\n";
    return EXIT_SUCCESS;
}
