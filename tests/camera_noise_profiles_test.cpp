/* This file is part of RawTherapee. SPDX-License-Identifier: GPL-3.0-or-later */
// Standalone: c++ -std=c++11 -Irtengine tests/camera_noise_profiles_test.cpp
//            rtengine/camera_noise_profiles.cc -o /tmp/camera_noise_profiles_test
#include "camera_noise_profiles.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace rtengine::raw_noise;

namespace {
void require(bool value, const char* message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void near(double actual, double expected)
{
    require(std::abs(actual - expected) <= 1e-12 * std::max(1.0, std::abs(expected)),
            "coefficient mismatch");
}

void rejected(const ReferenceCameraProfile& p, const char* reason)
{
    require(!p.matched && p.reason == reason, reason);
    for (unsigned c = 0; c < 4; ++c) {
        near(p.a[c], 0);
        near(p.b[c], 0);
    }
}
} // namespace

int main()
{
    const CameraNoiseDataInfo& info = cameraNoiseDataInfo();
    require(info.schema == 1 && info.inputRows == 931 &&
            info.profileCount == 832 && info.sampleCount == 10890, "compiled data counts");
    require(std::string(info.inputSHA256).size() == 64, "SHA256 metadata");
    auto p = lookupReferenceCameraProfile("canon", "EOS R5 C", 100);
    require(p.matched && p.generic && p.mode.empty() && p.reason.empty(), "exact ISO");
    near(p.a[0], 1.1324); near(p.a[1], 1.1087); near(p.a[2], 1.0688);
    near(p.b[0], 23.54); near(p.b[1], 22.82); near(p.b[2], 28.49);
    near(p.a[3], p.a[1]); near(p.b[3], p.b[1]);
    p = lookupReferenceCameraProfile("Canon", "Canon EOS R5 C", 150);
    require(p.matched, "interpolation lookup");
    near(p.a[0], (1.1324 + 2.2083) / 2);
    const double read = (std::sqrt(23.54) + std::sqrt(140.94)) / 2;
    near(p.b[0], read * read);
    p = lookupReferenceCameraProfile("Canon", "EOS R5 C", 25);
    near(p.a[0], 1.1218 / 2); near(p.b[0], 22.78 / 4);
    p = lookupReferenceCameraProfile("Canon", "EOS R5 C", 204800);
    near(p.a[0], 430.1486 * 2); near(p.b[0], 2895884.95 * 4);
    p = lookupReferenceCameraProfile("OLYMPUS", "E-620", 100);
    rejected(p, "no_match"); // camera exists but has no noise profile
    p = lookupReferenceCameraProfile("OLYMPUS", "E-P2", 100);
    require(p.matched, "maker spelling and monochannel expansion");
    near(p.a[0], 3.1613); near(p.a[2], p.a[0]); near(p.b[1], p.b[0]);
    p = lookupReferenceCameraProfile("Panasonic", "DMC-TZ81", 100);
    require(p.matched && p.model == "DMC-ZS60" &&
            p.aliases == "DMC-TZ80,DMC-TZ81,DMC-TZ85", "real aliases preserved");
    const auto mark = lookupReferenceCameraProfile("Canon", "EOS R6 Mark II", 100);
    const auto compact = lookupReferenceCameraProfile("CANON", "Canon EOS R6m2", 100);
    require(mark.matched && compact.matched && mark.model == compact.model, "Canon Mark folding");
    near(mark.a[0], compact.a[0]); near(mark.b[0], compact.b[0]);
    require(lookupReferenceCameraProfile("", "EOS R6 Mark II", 100).matched,
            "Canon folding on model fallback");
    rejected(lookupReferenceCameraProfile("", "", 100), "no_match");
    rejected(lookupReferenceCameraProfile("No", "Such Camera", 100), "no_match");
    rejected(lookupReferenceCameraProfile("Canon", "EOS R5 C", 0), "invalid_iso");
    rejected(lookupReferenceCameraProfile("Canon", "EOS R5 C", -100), "invalid_iso");
    rejected(lookupReferenceCameraProfile("Canon", "EOS R5 C",
             std::numeric_limits<double>::quiet_NaN()), "invalid_iso");
    rejected(lookupReferenceCameraProfile("Canon", "EOS R5 C",
             std::numeric_limits<double>::infinity()), "invalid_iso");

    const CameraNoiseSample samples[] = {
        {100, {1, 2, 3}, {4, 9, 16}}, {200, {3, 4, 5}, {16, 25, 36}}
    };
    CameraNoiseProfileRecord rows[] = {
        {"Acme", "Camera-1", "Alias,Camera-1", "12bit", false, samples, 2},
        {"Acme", "Camera-1", "Alias", "", true, samples, 2},
        {"Other", "Camera-1", "", "", true, samples, 2}
    };
    p = lookupReferenceCameraProfile("acme", "camera 1", 150, rows, 3);
    require(p.matched && p.generic && p.mode.empty(), "qualified make and generic priority");
    near(p.a[0], 2); near(p.b[0], 9); near(p.b[1], 16); near(p.b[2], 25);
    near(p.b[3], 16);
    p = lookupReferenceCameraProfile("Acme", "alias", 100, rows, 3);
    require(p.matched && p.generic, "alias identity and generic priority");
    rejected(lookupReferenceCameraProfile("", "Camera-1", 100, rows, 3), "ambiguous_profile");
    rejected(lookupReferenceCameraProfile("Unknown", "Camera-1", 100, rows, 3), "ambiguous_profile");
    require(lookupReferenceCameraProfile("Unknown", "Alias", 100, rows, 3).matched,
            "unambiguous model-only fallback");
    rows[2] = rows[1];
    rejected(lookupReferenceCameraProfile("Acme", "Camera-1", 100, rows, 3), "ambiguous_profile");
    rows[1].generic = false;
    rows[2].generic = false;
    rejected(lookupReferenceCameraProfile("Acme", "Alias", 100, rows, 3), "ambiguous_profile");
    // Single non-generic profile is usable as a reference, but mode is retained.
    p = lookupReferenceCameraProfile("Acme", "Camera1", 50, rows, 1);
    require(p.matched && !p.generic && p.mode == "12bit", "single mode retained");
    near(p.a[0], .5); near(p.b[0], 1);
    rows[1] = rows[0]; rows[1].model = "Camera2";
    rejected(lookupReferenceCameraProfile("Acme", "Alias", 100, rows, 2), "ambiguous_profile");
    rows[0].sampleCount = 1;
    p = lookupReferenceCameraProfile("Acme", "Camera1", 400, rows, 1);
    near(p.a[2], 12); near(p.b[2], 256);
    rejected(lookupReferenceCameraProfile("Acme", "Camera1",
             std::numeric_limits<double>::max(), rows, 1), "coefficient_overflow");
    CameraNoiseSample invalid[] = {samples[0], samples[0]};
    rows[0].samples = invalid; rows[0].sampleCount = 2;
    rejected(lookupReferenceCameraProfile("Acme", "Camera1", 100, rows, 1), "invalid_profile");
    invalid[1] = samples[1]; invalid[1].b[1] = -1;
    rejected(lookupReferenceCameraProfile("Acme", "Camera1", 100, rows, 1), "invalid_profile");
    rejected(lookupReferenceCameraProfile("Acme", "Camera1", 100, nullptr, 0), "no_match");
    std::cout << "camera_noise_profiles_test: all checks passed\n";
}
