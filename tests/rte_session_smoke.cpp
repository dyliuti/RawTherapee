// Standalone native RawTherapee Session API smoke test; see README.md.
#include "raw_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
static_assert(RTE_API_VERSION == 2, "legacy API version must not change for Session V3");
static_assert(RTE_RAW_SESSION_API_VERSION == 5, "reference map requires Session V5");
static_assert(offsetof(rte_raw_noise_info, struct_size) == 0
              && offsetof(rte_raw_noise_info, iso) == 32,
              "noise info ABI prefix changed");
static_assert(offsetof(rte_linear_rgb_view, struct_size) == 0
              && offsetof(rte_linear_rgb_view, width) == 4
              && offsetof(rte_linear_rgb_view, height) == 8
              && offsetof(rte_linear_rgb_view, channels) == 12
              && offsetof(rte_linear_rgb_view, data) == 16,
              "V1 view prefix layout changed");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(rte_raw_noise_info) == 2632
              && offsetof(rte_raw_noise_info, reference_a) == 56
              && offsetof(rte_raw_noise_info, camera_to_working) == 320
              && offsetof(rte_raw_noise_info, make) == 392,
              "V3 64-bit noise snapshot layout changed");
static_assert(sizeof(rte_linear_rgb_view) == 40
              && offsetof(rte_linear_rgb_view, count) == 24
              && offsetof(rte_linear_rgb_view, row_stride_bytes) == 32,
              "V1 64-bit view layout changed");
#endif

void require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}

template<class F> int timed(const char* label, F call)
{
    std::cout << "BEGIN " << label << std::endl;
    const auto start = Clock::now();
    const int rc = call();
    std::cout << label << " rc=" << rc << " ms="
              << std::chrono::duration<double, std::milli>(Clock::now() - start).count()
              << std::endl;
    return rc;
}

uint64_t hashBytes(const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool empty(const rte_image_buffer& image)
{
    return !image.data && !image.size && !image.width && !image.height;
}

struct Image {
    rte_image_buffer value{};
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image() { rte_image_buffer_free(&value); }
    void release()
    {
        require(rte_image_buffer_free(&value) == 0 && empty(value), "buffer free/clear failed");
    }
    void report(const char* label) const
    {
        const auto& v = value;
        require(v.data && v.width > 0 && v.height > 0 && v.size > 0,
                std::string(label) + ": invalid image");
        require(uint64_t(v.size) == uint64_t(v.width) * uint64_t(v.height) * 4,
                std::string(label) + ": invalid RGBA byte count");
        std::cout << label << " dimensions=" << v.width << 'x' << v.height
                  << " bytes=" << v.size << " fnv1a64=0x" << std::hex
                  << hashBytes(v.data, size_t(v.size)) << std::dec << std::endl;
    }
};

struct Session {
    rte_raw_session* value = nullptr;
    ~Session() { if (value) rte_raw_session_close(value); }
};

rte_raw_noise_info noiseInfo(const Session& session)
{
    rte_raw_noise_info info{};
    info.struct_size = sizeof(info);
    require(rte_raw_session_get_noise_info(session.value, &info) == 0,
            "noise snapshot query failed");
    return info;
}

void sameNoise(const rte_raw_noise_info& a, const rte_raw_noise_info& b)
{
    require(std::memcmp(&a, &b, sizeof(a)) == 0, "immutable noise snapshot changed");
}

rte_reference_noise_model referenceInfo(const Session& s)
{
    rte_reference_noise_model r{};
    r.struct_size = sizeof(r);
    require(rte_raw_session_get_reference_noise_model(s.value, &r) == 0, "reference query failed");
    require(r.version == 1 && r.stage3_gain == 1, "reference version/gain contract");
    return r;
}

// Reports all bytes, including alpha. RGB-only mean is also useful diagnostically.
uint64_t compare(const char* label, const rte_image_buffer& baseline,
                 const rte_image_buffer& image, bool exact)
{
    require(baseline.width == image.width && baseline.height == image.height
            && baseline.size == image.size, std::string(label) + ": dimensions differ");
    const auto* a = static_cast<const unsigned char*>(baseline.data);
    const auto* b = static_cast<const unsigned char*>(image.data);
    uint64_t sum = 0, rgb_sum = 0, changed = 0;
    size_t first = size_t(image.size);
    int maximum = 0;
    for (size_t i = 0; i < size_t(image.size); ++i) {
        const int d = std::abs(int(a[i]) - int(b[i]));
        sum += d;
        if (i % 4 != 3) rgb_sum += d;
        maximum = std::max(maximum, d);
        if (d) {
            ++changed;
            if (first == size_t(image.size)) first = i;
        }
    }
    std::cout << label << " max_abs=" << maximum
              << " mean_abs_rgba=" << double(sum) / image.size
              << " mean_abs_rgb=" << double(rgb_sum) / (image.size / 4 * 3)
              << " changed_bytes=" << changed << std::endl;
    if (changed) {
        std::cout << "first_difference x=" << (first / 4) % image.width
                  << " y=" << (first / 4) / image.width << " channel=" << first % 4
                  << " baseline=" << int(a[first]) << " actual=" << int(b[first]) << std::endl;
    }
    require(!exact || changed == 0, std::string(label) + ": NOT byte-exact; no tolerance applied");
    return changed;
}

void finish(Session& session, const char* label, const float* data,
            uint64_t count, Image& image)
{
    require(empty(image.value), "finish output must initially be empty");
    const auto before = noiseInfo(session);
    const auto reference = referenceInfo(session);
    require(timed(label, [&] {
        return rte_raw_session_finish(session.value, data, count, &image.value);
    }) == 0, std::string(label) + " failed");
    image.report(label);
    sameNoise(before, noiseInfo(session));
    const auto after = referenceInfo(session);
    require(std::memcmp(&reference, &after, sizeof(after)) == 0, "reference snapshot changed on finish");
}

void reject(Session& session, const char* label, const float* data, uint64_t count)
{
    // Non-owning sentinels test that ALL output fields are cleared on failure.
    // Never free the sentinel if a broken implementation leaves it untouched.
    unsigned char sentinel = 0;
    rte_image_buffer out{&sentinel, 17, 19, 23};
    const auto before = noiseInfo(session);
    const int rc = timed(label, [&] {
        return rte_raw_session_finish(session.value, data, count, &out);
    });
    const bool cleared = empty(out);
    std::cout << label << " out_empty=" << cleared << std::endl;
    if (out.data && out.data != &sentinel) rte_image_buffer_free(&out);
    require(rc == -1 && cleared, std::string(label) + ": expected -1 and empty output");
    sameNoise(before, noiseInfo(session));
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf << std::fixed << std::setprecision(9);
    if (argc != 3) {
        std::cerr << "Usage: rte_session_smoke RAW_FILE RESOURCE_DIRECTORY\n";
        return 2;
    }
    try {
        require(rte_api_version() == RTE_API_VERSION, "header/library API version mismatch");
        require(rte_raw_session_api_version() == RTE_RAW_SESSION_API_VERSION,
                "header/library Session ABI version mismatch");
        for (auto open : {rte_raw_session_open, rte_raw_source_session_open}) {
            int sentinel = 0;
            auto* handle = reinterpret_cast<rte_raw_session*>(&sentinel);
            require(open(argv[1], nullptr, &handle) == -2 && !handle,
                    "open before init not rejected/cleared");
        }
        rte_engine_options engine{};
        engine.resource_path = argv[2];
        require(timed("engine_init", [&] { return rte_engine_init(&engine); }) == 0, "init failed");

        // Not passed to decode/session: NR is not wired into these APIs. Do not
        // confuse the UI default color_amount=25 with enabled processing.
        rte_denoise_options nr{};
        rte_denoise_options_default(&nr);
        std::cout << "NR defaults enabled=" << nr.enabled << " luma=" << nr.luminance_amount
                  << " color=" << nr.color_amount << "; no NR override/enable\n";
        require(nr.enabled == 0 && nr.luminance_amount == 0, "unexpected NR defaults");

        rte_decode_options options{};
        options.struct_size = sizeof(options);
        options.scale = RTE_SCALE_FULL;
        options.demosaic = RTE_DEMOSAIC_DEFAULT;
        // One legacy decode, then one open per mode (never held concurrently).
        Image baseline;
        require(timed("legacy_decode_FULL", [&] {
            return rte_decode(argv[1], &options, &baseline.value);
        }) == 0, "legacy decode failed");
        baseline.report("legacy_decode_FULL");
        rte_raw_session_stage invalid_stage = RTE_RAW_SESSION_STAGE_POST_TRANSFORM;
        require(rte_raw_session_get_stage(nullptr, &invalid_stage) == -1
                && invalid_stage == RTE_RAW_SESSION_STAGE_UNKNOWN, "invalid stage query not cleared");
        require(rte_raw_session_get_stage(nullptr, nullptr) == -1, "null stage output not rejected");

        for (bool source : {false, true}) {
        std::cout << "MODE " << (source ? "V2_PROPHOTO_SOURCE" : "V1_POST_TRANSFORM") << std::endl;
        const auto open = source ? rte_raw_source_session_open : rte_raw_session_open;
        auto rejectOpen = [&](const char* filename, const rte_decode_options* opt, int expected) {
            // Valid non-owning address as sentinel, never an actual session.
            int sentinel = 0;
            auto* handle = reinterpret_cast<rte_raw_session*>(&sentinel);
            const int rc = open(filename, opt, &handle);
            require(rc == expected && !handle, "invalid open not rejected/cleared");
        };
        rejectOpen(nullptr, &options, -1);
        rejectOpen("", &options, -1);
        require(open(argv[1], &options, nullptr) == -1, "null open output not rejected");
        rte_decode_options bad = options;
        bad.struct_size = sizeof(bad) - 1;
        rejectOpen(argv[1], &bad, -1);
        bad = options; bad.scale = static_cast<rte_scale_mode>(999);
        rejectOpen(argv[1], &bad, -2);
        bad = options; bad.demosaic = RTE_DEMOSAIC_NONE + 1;
        rejectOpen(argv[1], &bad, -1);
        bad = options; bad.force_raw = 2;
        rejectOpen(argv[1], &bad, -1);
        bad = options; bad.reserved[1] = 1;
        rejectOpen(argv[1], &bad, -1);
        rte_lens_options lens{};
        rte_lens_options_default(&lens);
        bad = options; bad.lens = &lens;
        lens.struct_size = sizeof(lens) - 1;
        rejectOpen(argv[1], &bad, -1);
        rte_lens_options_default(&lens);
        lens.mode = RTE_LENS_MANUAL; // required names missing
        rejectOpen(argv[1], &bad, -1);
        Session session;
        require(timed("session_open", [&] {
            return open(argv[1], &options, &session.value);
        }) == 0 && session.value, "session open failed");
        rte_raw_session_stage stage = RTE_RAW_SESSION_STAGE_UNKNOWN;
        require(rte_raw_session_get_stage(session.value, &stage) == 0
                && stage == (source ? RTE_RAW_SESSION_STAGE_PROPHOTO_SOURCE
                                    : RTE_RAW_SESSION_STAGE_POST_TRANSFORM), "wrong session stage");
        std::cout << "view_stage=" << int(stage) << std::endl;

        require(rte_raw_session_get_noise_info(session.value, nullptr) == -1,
                "null noise output not rejected");
        // Small actual allocation + canary: never write sizeof(full struct).
        struct alignas(rte_raw_noise_info) ShortNoise {
            uint32_t size; uint32_t field; uint32_t guard;
        } shortNoise{
            2 * sizeof(uint32_t), 123, 0x12345678};
        require(rte_raw_session_get_noise_info(session.value,
                    reinterpret_cast<rte_raw_noise_info*>(&shortNoise)) == -1
                && shortNoise.size == 8 && !shortNoise.field && shortNoise.guard == 0x12345678,
                "undersized noise ABI overwrote canary/not cleared");
        shortNoise.size = 0; shortNoise.field = 123;
        require(rte_raw_session_get_noise_info(session.value,
                    reinterpret_cast<rte_raw_noise_info*>(&shortNoise)) == -1
                && !shortNoise.size && shortNoise.field == 123,
                "zero-size noise ABI wrote output");
        rte_raw_noise_info invalidNoise;
        std::memset(&invalidNoise, 0x5a, sizeof(invalidNoise));
        invalidNoise.struct_size = sizeof(invalidNoise);
        require(rte_raw_session_get_noise_info(nullptr, &invalidNoise) == -1,
                "null noise handle not rejected");
        rte_raw_noise_info clearedNoise{};
        clearedNoise.struct_size = sizeof(clearedNoise);
        sameNoise(invalidNoise, clearedNoise);
        int fakeHandle = 0;
        require(rte_raw_session_get_noise_info(
                    reinterpret_cast<rte_raw_session*>(&fakeHandle), &invalidNoise) == -1,
                "unregistered noise handle not rejected");
        sameNoise(invalidNoise, clearedNoise);
        const auto noise = noiseInfo(session);
        const auto reference = referenceInfo(session);
        require(rte_raw_session_get_reference_noise_model(session.value, nullptr)==-1, "null reference output");
        struct alignas(rte_reference_noise_model) ShortReference {
            uint32_t size, field, guard;
        } shortReference{8,123,0x12345678};
        require(rte_raw_session_get_reference_noise_model(session.value,
                    reinterpret_cast<rte_reference_noise_model*>(&shortReference))==-1 &&
                shortReference.size==8 && shortReference.field==0 && shortReference.guard==0x12345678,
                "short reference canary");
        shortReference.size=0; shortReference.field=123;
        require(rte_raw_session_get_reference_noise_model(session.value,
                    reinterpret_cast<rte_reference_noise_model*>(&shortReference))==-1 &&
                shortReference.field==123, "zero reference size writes");
        auto invalidReference=reference;
        rte_reference_noise_model clearedReference{}; clearedReference.struct_size=sizeof(clearedReference);
        int fakeReference=0;
        for (const auto* handle : {static_cast<const rte_raw_session*>(nullptr),
                                  reinterpret_cast<const rte_raw_session*>(&fakeReference)}) {
            require(rte_raw_session_get_reference_noise_model(handle,&invalidReference)==-1 &&
                    std::memcmp(&invalidReference,&clearedReference,sizeof(invalidReference))==0,
                    "invalid reference handle not cleared");
        }
        struct ExtendedReference { rte_reference_noise_model info; unsigned char tail[16]; } extraReference;
        std::memset(&extraReference,0x5a,sizeof(extraReference));
        extraReference.info.struct_size=sizeof(extraReference);
        require(rte_raw_session_get_reference_noise_model(session.value,&extraReference.info)==0,
                "larger reference rejected");
        for (auto b : extraReference.tail) require(b==0x5a,"reference tail overwritten");
        extraReference.info.struct_size=sizeof(reference);
        require(std::memcmp(&reference,&extraReference.info,sizeof(reference))==0,"reference repeated query mismatch");
        require(!reference.spatially_resampled, "FULL unexpectedly resampled");
        if (noise.profile_matched && noise.color_matrix_valid) {
            require(reference.reference_usable && !noise.working_model_valid,
                    "noncalibrated direct matrix reference must be usable");
        }
        for (unsigned c=0;c<4;++c) {
            require(reference.normalized_a[c] == noise.reference_a[c]/65535.0 &&
                    reference.normalized_b[c] == noise.reference_b[c]/(65535.0*65535.0), "normalized reference contract");
            if (reference.reference_usable) require(reference.channel_gain[c] ==
                noise.preprocessing_scale[c]*noise.render_multiplier[c==3?1:c]*
                (noise.decoder_white_dn[c]-noise.decoder_black_dn[c])/65535.0, "observed channel gain contract");
            std::cout << "normalized[" << c << "] A=" << std::setprecision(15) << reference.normalized_a[c]
                      << " B=" << reference.normalized_b[c] << " gain=" << reference.channel_gain[c] << '\n';
        }
        if (reference.reference_usable) {
            const double gains[3]={reference.channel_gain[0],
                reference.channel_gain[1]*0.5+reference.channel_gain[3]*0.5,reference.channel_gain[2]};
            for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j)
                require(reference.camera_to_working[i*3+j]==noise.camera_to_working[i*3+j]*gains[j],
                        "observed M diag(gain) contract");
        }
        std::cout << "reference_usable=" << reference.reference_usable << " stage3Gain=" << reference.stage3_gain
                  << " hash=" << std::hex << reference.snapshot_hash << std::dec << " reason=" << reference.reason << '\n';
        for (unsigned i=0;i<3;++i) std::cout << "reference_map " << reference.camera_to_working[i*3]
            << ' ' << reference.camera_to_working[i*3+1] << ' ' << reference.camera_to_working[i*3+2] << '\n';
        struct ExtendedNoise { rte_raw_noise_info info; unsigned char tail[16]; } extended;
        std::memset(&extended, 0x5a, sizeof(extended));
        extended.info.struct_size = sizeof(extended);
        require(rte_raw_session_get_noise_info(session.value, &extended.info) == 0
                && extended.info.struct_size == sizeof(extended), "larger noise ABI rejected");
        for (auto byte : extended.tail) require(byte == 0x5a, "larger noise ABI tail overwritten");
        extended.info.struct_size = sizeof(noise);
        sameNoise(noise, extended.info);
        std::memset(&extended.info, 0xa5, sizeof(extended.info));
        sameNoise(noise, noiseInfo(session)); // caller edits never mutate owner
        require(!noise.working_model_valid && !noise.sensor_calibration_valid
                && noise.profile_quality != RTE_NOISE_PROFILE_CALIBRATED,
                "uncalibrated working model falsely certified");
        require(noise.raw_code_scale == 65535 && noise.render_metadata_valid
                && noise.render_skip == 1 && std::string(noise.working_profile) == "ProPhoto",
                "preflush render snapshot or conversion observations lost");
        require(noise.reason[0] && noise.profile_reason[0] && noise.sensor_reason[0]
                && noise.fallback[0], "noise diagnostics missing");
        for (const auto* text : {noise.make, noise.model, noise.profile_make, noise.profile_model,
                                noise.profile_mode}) {
            require(std::memchr(text, 0, 128), "unterminated noise identity");
        }
        for (const auto* text : {noise.reason, noise.profile_reason, noise.reference_reason,
                                noise.sensor_reason, noise.color_reason, noise.fallback}) {
            require(std::memchr(text, 0, 256), "unterminated noise diagnostic");
        }
        require(std::memchr(noise.working_profile, 0, 64), "unterminated working profile");
        if (std::string(argv[1]).find("IMG_6771") != std::string::npos) {
            require(noise.profile_matched && noise.profile_quality == RTE_NOISE_PROFILE_REFERENCE,
                    "IMG_6771 must export Reference match despite working-invalid");
            require((std::string(noise.model)=="EOS R" || std::string(noise.model)=="Canon EOS R") && reference.reference_usable,
                    "Canon EOS R FULL reference regression");
        }
        for (size_t c = 0; c < 4; ++c) {
            require(std::isfinite(noise.decoder_black_dn[c])
                    && std::isfinite(noise.decoder_white_dn[c])
                    && noise.decoder_white_dn[c] > noise.decoder_black_dn[c]
                    && std::isfinite(noise.preprocessing_black_dn[c])
                    && std::isfinite(noise.preprocessing_white_dn[c])
                    && std::isfinite(noise.preprocessing_scale[c]) && noise.preprocessing_scale[c] > 0
                    && noise.preprocessing_white_dn[c] > noise.preprocessing_black_dn[c],
                    "preprocessing observations lost");
            if (noise.profile_matched) {
                require(std::isfinite(noise.reference_a[c]) && noise.reference_a[c] > 0
                        && std::isfinite(noise.reference_b[c]) && noise.reference_b[c] >= 0,
                        "reference coefficients unavailable");
            }
        }
        if (noise.profile_matched) {
            require(noise.reference_a[1] == noise.reference_a[3]
                    && noise.reference_b[1] == noise.reference_b[3], "G2 reference mismatch");
            require(noise.iso > 0 && std::isfinite(noise.iso)
                    && noise.profile_iso == noise.iso && noise.profile_make[0]
                    && noise.profile_model[0], "reference ISO/identity missing");
        }
        for (double multiplier : noise.render_multiplier) {
            require(std::isfinite(multiplier) && multiplier > 0, "render multiplier lost");
        }
        for (double value : noise.camera_to_working) {
            require(std::isfinite(value) && (noise.color_matrix_valid || value == 0),
                    "invalid matrix export");
        }
        require(noise.color_matrix_valid || noise.color_reason[0], "missing conversion reason");
        std::cout << "noise quality=" << noise.profile_quality << " matched=" << noise.profile_matched
                  << " working_valid=" << noise.working_model_valid << " ISO=" << noise.iso
                  << " rawCodeScale=" << noise.raw_code_scale
                  << " render_valid=" << noise.render_metadata_valid
                  << " matrix_valid=" << noise.color_matrix_valid
                  << " reason=" << noise.reason << " color_reason=" << noise.color_reason
                  << " profile=" << noise.profile_make << '/' << noise.profile_model << std::endl;
        for (size_t c = 0; c < 4; ++c) {
            std::cout << "reference[" << c << "] A=" << noise.reference_a[c]
                      << " B=" << noise.reference_b[c] << std::endl;
        }

        rte_linear_rgb_view invalid_view{};
        invalid_view.struct_size = sizeof(invalid_view);
        invalid_view.width = 123;
        require(rte_raw_session_get_linear(nullptr, &invalid_view) == -1
                && invalid_view.struct_size == sizeof(invalid_view)
                && !invalid_view.width && !invalid_view.height && !invalid_view.channels
                && !invalid_view.data && !invalid_view.count && !invalid_view.row_stride_bytes,
                "invalid get not rejected/cleared");
        alignas(rte_linear_rgb_view) uint32_t short_view = sizeof(uint32_t);
        require(rte_raw_session_get_linear(session.value,
                    reinterpret_cast<rte_linear_rgb_view*>(&short_view)) == -1
                && short_view == sizeof(uint32_t), "undersized view not rejected");
        require(rte_raw_session_get_linear(session.value, nullptr) == -1,
                "null view not rejected");

        rte_linear_rgb_view view{};
        view.struct_size = sizeof(view);
        require(timed("get_linear", [&] {
            return rte_raw_session_get_linear(session.value, &view);
        }) == 0, "get linear failed");
        require(view.data && view.width && view.height && view.channels == 3
                && view.count == uint64_t(view.width) * view.height * 3
                && view.row_stride_bytes == uint64_t(view.width) * 3 * sizeof(float)
                && view.count <= std::numeric_limits<size_t>::max() / sizeof(float),
                "invalid linear layout");
        const size_t linear_bytes = size_t(view.count) * sizeof(float);
        const uint64_t linear_hash = hashBytes(view.data, linear_bytes);
        float low = std::numeric_limits<float>::max(), high = -low;
        for (uint64_t i = 0; i < view.count; ++i) {
            require(std::isfinite(view.data[i]), "nonfinite original linear data");
            low = std::min(low, view.data[i]);
            high = std::max(high, view.data[i]);
        }
        std::cout << "linear dimensions=" << view.width << 'x' << view.height
                  << " count=" << view.count << " stride=" << view.row_stride_bytes
                  << " bytes=" << linear_bytes << " range=[" << low << ',' << high
                  << "] fnv1a64=0x" << std::hex << linear_hash << std::dec << std::endl;

        Image output;
        finish(session, "finish_null_0", nullptr, 0, output);
        // Source parity here is conditional on this fixture's legacy NR being
        // disabled; it is NOT an equivalence claim for arbitrary rule sets.
        compare("legacy_vs_null", baseline.value, output.value, true);
        // Keep only ONE RGBA baseline, plus ONE current output, at any time.
        baseline.release();
        std::swap(baseline.value, output.value);
        auto checkOriginal = [&](const char* label, const float* data, uint64_t count) {
            finish(session, label, data, count, output);
            compare(label, baseline.value, output.value, true);
            output.release();
            require(hashBytes(view.data, linear_bytes) == linear_hash,
                    std::string(label) + ": borrowed linear data mutated");
        };
        checkOriginal("finish_null_repeat", nullptr, 0);
        checkOriginal("finish_original_interleaved", view.data, view.count);
        checkOriginal("finish_original_repeat", view.data, view.count);

        // One editable float copy only; never write through the borrowed pointer.
        std::vector<float> edited(view.data, view.data + size_t(view.count));
        const float gain = std::exp2(0.5f);
        std::cout << "edit exposure_EV=0.5 linear_gain=" << gain << std::endl;
        for (float& v : edited) v *= gain;
        finish(session, "finish_exposure_plus_0_5", edited.data(), view.count, output);
        require(compare("exposure_vs_original", baseline.value, output.value, false) > 0,
                "exposure edit had no visible effect");
        output.release();
        checkOriginal("finish_original_after_edit", view.data, view.count);
        checkOriginal("finish_null_after_edit", nullptr, 0);

        reject(session, "invalid_null_nonzero", nullptr, view.count);
        reject(session, "invalid_short_count", edited.data(), view.count - 1);
        reject(session, "invalid_long_count", edited.data(), view.count + 1);
        reject(session, "invalid_pointer_zero_count", edited.data(), 0);
        const float saved = edited.back();
        edited.back() = std::numeric_limits<float>::quiet_NaN();
        reject(session, "invalid_NaN_last_element", edited.data(), view.count);
        edited.back() = std::numeric_limits<float>::infinity();
        reject(session, "invalid_Inf_last_element", edited.data(), view.count);
        edited.back() = saved;
        require(timed("invalid_null_out", [&] {
            return rte_raw_session_finish(session.value, nullptr, 0, nullptr);
        }) == -1, "null out not rejected");
        std::vector<float>().swap(edited);
        checkOriginal("finish_original_after_invalid", view.data, view.count);
        rte_linear_rgb_view again{};
        again.struct_size = sizeof(again);
        require(rte_raw_session_get_linear(session.value, &again) == 0
                && again.data == view.data && again.width == view.width && again.height == view.height
                && again.channels == view.channels && again.count == view.count
                && again.row_stride_bytes == view.row_stride_bytes
                && hashBytes(again.data, linear_bytes) == linear_hash, "borrowed view not stable");
        rte_raw_session_stage stage_after = RTE_RAW_SESSION_STAGE_UNKNOWN;
        require(rte_raw_session_get_stage(session.value, &stage_after) == 0
                && stage_after == stage, "session stage changed across finishes");
        sameNoise(noise, noiseInfo(session));

        const int close_rc = timed("session_close", [&] { return rte_raw_session_close(session.value); });
        session.value = nullptr; // Never access/reclose a closed handle, even on failure.
        require(close_rc == 0, "close failed");
        require(noise.raw_code_scale == 65535 && noise.reason[0],
                "owned noise snapshot not usable after close");
        // The RGBA allocation remains valid independently of the closed session.
        baseline.report("baseline_after_close");
        require(rte_raw_session_close(nullptr) == 0, "close(nullptr) failed");
        }
        baseline.release();
        std::cout << "PASS: V1 + V2 source exact comparisons (fixture legacy NR off), "
                     "V3 noise reference/render snapshots stable, repeats, edit isolation, validation, close\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
