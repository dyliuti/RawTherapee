// Independent V4 preview regression, no production/extra build changes.
#include "raw_engine.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
static_assert(RTE_RAW_SESSION_API_VERSION == 5 && RTE_API_VERSION == 2, "ABI version");
static_assert(sizeof(rte_linear_rgb_view) == 40, "existing arm64 linear ABI");
static_assert(sizeof(rte_raw_session_dimensions) == 32, "V4 geometry ABI");

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Image {
    rte_image_buffer v{};
    ~Image() { rte_image_buffer_free(&v); }
};
struct Session {
    rte_raw_session* v = nullptr;
    ~Session() { rte_raw_session_close(v); }
};
struct Fixture {
    fs::path dir, file, hidden;
    explicit Fixture(const fs::path& original) {
        dir = fs::temp_directory_path() / ("rte-preview-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(dir);
        file = dir / original.filename();
        hidden = dir / "unavailable.raw";
        fs::copy_file(original, file);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(dir, ec); }
};
void equal(const rte_image_buffer& a, const rte_image_buffer& b, bool exact) {
    check(a.width == b.width && a.height == b.height && a.size == b.size, "output geometry differs");
    const auto* p = static_cast<const unsigned char*>(a.data);
    const auto* q = static_cast<const unsigned char*>(b.data);
    size_t changed = 0; int maximum = 0; uint64_t sum = 0;
    for (int i = 0; i < a.size; ++i) {
        int d = std::abs(int(p[i]) - int(q[i]));
        changed += d != 0; maximum = std::max(maximum, d); sum += d;
    }
    std::cout << "changed_bytes=" << changed << " max_abs=" << maximum
              << " mean_abs=" << double(sum) / a.size << std::endl;
    check(!exact || !changed, "byte mismatch (no tolerance)");
}
void finish(Session& s, Image& out, const float* p = nullptr, uint64_t n = 0) {
    rte_reference_noise_model before{}; before.struct_size=sizeof(before);
    check(rte_raw_session_get_reference_noise_model(s.v, &before)==0, "reference query failed");
    check(!before.reference_usable && before.spatially_resampled && before.resolution_scale<1,
          "preview must reject unmodelled spatial noise");
    rte_raw_noise_info evidence{}; evidence.struct_size=sizeof(evidence);
    check(rte_raw_session_get_noise_info(s.v,&evidence)==0, "preview evidence query failed");
    check(before.stage3_gain==1 && (!evidence.profile_matched || before.normalized_a[0]>0),
          "preview lost reference evidence");
    std::cout << "preview reference_usable=" << before.reference_usable << " scale=" << before.resolution_scale
              << " hash=" << before.snapshot_hash << " reason=" << before.reason << '\n';
    check(rte_raw_session_finish(s.v, p, n, &out.v) == 0, "finish failed");
    rte_reference_noise_model after{}; after.struct_size=sizeof(after);
    check(rte_raw_session_get_reference_noise_model(s.v, &after)==0 &&
          std::memcmp(&before,&after,sizeof(before))==0, "preview reference changed on finish");
    check(out.v.data && out.v.width > 0 && out.v.height > 0
          && out.v.size == int64_t(out.v.width) * out.v.height * 4, "invalid RGBA");
}
uint64_t hash(const float* p, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    const auto* bytes = reinterpret_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n * sizeof(float); ++i) h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Usage: rte_session_preview_smoke RAW RESOURCE_DIRECTORY\n"; return 2; }
    std::cout << std::unitbuf;
    try {
        check(rte_raw_session_api_version() == RTE_RAW_SESSION_API_VERSION, "wrong library version");
        rte_engine_options engine{}; engine.resource_path = argv[2];
        check(rte_engine_init(&engine) == 0, "init failed");
        for (auto scale : {RTE_SCALE_2K, RTE_SCALE_3K, RTE_SCALE_4K}) {
            rte_decode_options options{}; options.struct_size = sizeof(options); options.scale = scale;
            Image legacy;
            check(rte_decode(argv[1], &options, &legacy.v) == 0, "legacy decode failed");
            const int edge = scale == RTE_SCALE_2K ? 2000 : scale == RTE_SCALE_3K ? 3000 : 4000;
            for (bool source : {false, true}) {
                std::cout << "BEGIN edge=" << edge << " mode=" << (source ? "V2" : "V1") << std::endl;
                Fixture fixture(argv[1]);
                Session session;
                auto open = source ? rte_raw_source_session_open : rte_raw_session_open;
                check(open(fixture.file.c_str(), &options, &session.v) == 0, "open failed");
                // Only the temporary copy is hidden. Original fixture is untouched.
                fs::rename(fixture.file, fixture.hidden);
                check(!fs::exists(fixture.file), "fixture is still readable");
                rte_linear_rgb_view view{}; view.struct_size = sizeof(view);
                check(rte_raw_session_get_linear(session.v, &view) == 0, "get linear failed");
                check(view.count == uint64_t(view.width) * view.height * 3
                      && view.row_stride_bytes == uint64_t(view.width) * 12, "linear layout");
                rte_raw_session_dimensions g{}; g.struct_size = sizeof(g);
                check(rte_raw_session_get_dimensions(session.v, &g) == 0, "geometry query failed");
                check(g.retained_width == view.width && g.retained_height == view.height
                      && g.native_width > g.retained_width && g.native_height > g.retained_height
                      && g.resolution_scale > 0 && g.resolution_scale < 1, "not a real downsample");
                std::cout << "native=" << g.native_width << 'x' << g.native_height
                          << " retained=" << g.retained_width << 'x' << g.retained_height
                          << " spatial_scale=" << g.resolution_scale << std::endl;
                struct Extended { rte_raw_session_dimensions g; unsigned char tail[16]; } large;
                std::memset(&large, 0xa5, sizeof(large)); large.g.struct_size = sizeof(large);
                check(rte_raw_session_get_dimensions(session.v, &large.g) == 0, "oversized geometry");
                for (auto c : large.tail) check(c == 0xa5, "oversized tail overwritten");
                uint32_t tiny[2] = {4, 0x12345678};
                check(rte_raw_session_get_dimensions(session.v,
                    reinterpret_cast<rte_raw_session_dimensions*>(tiny)) == -1
                    && tiny[1] == 0x12345678, "undersized write");
                rte_raw_session_dimensions invalid;
                std::memset(&invalid, 0xa5, sizeof(invalid)); invalid.struct_size = sizeof(invalid);
                check(rte_raw_session_get_dimensions(nullptr, &invalid) == -1
                      && invalid.struct_size == sizeof(invalid) && !invalid.native_width
                      && !invalid.retained_width && !invalid.resolution_scale, "failure not cleared");
                const auto originalHash = hash(view.data, view.count);
                Image baseline; finish(session, baseline);
                std::cout << "output=" << baseline.v.width << 'x' << baseline.v.height << std::endl;
                check(std::max(baseline.v.width, baseline.v.height) <= edge, "preview too large");
                std::cout << (source ? "V2 vs legacy (diagnostic only): " : "V1 vs legacy: ");
                equal(legacy.v, baseline.v, !source);
                { Image repeat; finish(session, repeat); equal(baseline.v, repeat.v, true); }
                { Image original; finish(session, original, view.data, view.count); equal(baseline.v, original.v, true); }
                std::vector<float> edited(view.data, view.data + view.count);
                for (auto& f : edited) { check(std::isfinite(f), "nonfinite linear"); f *= std::sqrt(2.f); }
                { Image edit; finish(session, edit, edited.data(), edited.size());
                  check(edit.v.size == baseline.v.size && std::memcmp(edit.v.data, baseline.v.data, edit.v.size), "edit had no effect"); }
                { Image restored; finish(session, restored); equal(baseline.v, restored.v, true); }
                { Image restored; finish(session, restored, view.data, view.count); equal(baseline.v, restored.v, true); }
                check(hash(view.data, view.count) == originalHash, "retained buffer changed");
                rte_raw_session_dimensions after{}; after.struct_size = sizeof(after);
                check(rte_raw_session_get_dimensions(session.v, &after) == 0
                      && !std::memcmp(&g, &after, sizeof(g)), "geometry mutated");
                std::cout << "PASS size/repeat/edit/restore/no-reread" << std::endl;
            }
        }
        std::cout << "PASS preview smoke (V2 parity explicitly NOT required)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
