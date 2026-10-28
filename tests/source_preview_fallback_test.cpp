// Synthetic TIFF with EXIF DefaultCrop: exercises the public cached-fallback
// rejection, not a real-camera calibration or RAW geometry claim.
#include "raw_engine.h"
#include <exiv2/exiv2.hpp>
#include <tiffio.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
void check(bool ok, const char* msg);
#ifdef RTE_GEOMETRY_ENGINE_FIXTURES
#include "working_image_session.h"
#include "rtengine.h"
#include "procparams.h"
#include "iimage.h"
#include <memory>

void processor_fixtures(const char* filename)
{
    int error = 0;
    auto* image = rtengine::InitialImage::load(filename, false, &error, nullptr);
    check(!error && image, "internal fixture load");
    struct Release { void operator()(rtengine::InitialImage* p) const { p->decreaseRef(); } };
    std::unique_ptr<rtengine::InitialImage, Release> owner(image);
    rtengine::procparams::ProcParams p;
    p.icm.workingProfile = "ProPhoto";
    p.icm.outputProfile = "sRGB";
    p.icm.inputProfile = "";
    p.toneCurve.histmatching = false;
    p.resize.enabled = true; p.resize.width = p.resize.height = 32;
    p.resize.allowUpscaling = false;
    p.crop.enabled = true; p.crop.x = 4; p.crop.y = 6; p.crop.w = 16; p.crop.h = 12;
    auto create = [&](const rtengine::procparams::ProcParams& params) {
        auto* job = rtengine::ProcessingJob::create(image, params, true);
        error = 0;
        return rtengine::createProPhotoSourceSession(job, error, nullptr, false);
    };
    // Same loaded image for every custom job: no repeated load/decode.
    {
        std::unique_ptr<rtengine::WorkingImageSession, decltype(&rtengine::destroyWorkingImageSession)>
            session(create(p), rtengine::destroyWorkingImageSession);
        check(session && !error, "small crop no-upscale source create");
        auto g = rtengine::getWorkingImageSessionGeometry(session.get());
        check(g.nativeWidth == 64 && g.nativeHeight == 48 && g.retainedWidth == 64
            && g.retainedHeight == 48 && g.resolutionScale == 1.0, "small crop canvas must not upscale");
        std::unique_ptr<rtengine::IImagefloat> result(
            rtengine::finishWorkingImageSession(session.get(), nullptr, 0, error));
        check(result && !error && result->getWidth() == 16 && result->getHeight() == 12,
              "small crop finish geometry");
    }
    {
        auto down = p; down.crop.x = 8; down.crop.y = 6; down.crop.w = 48; down.crop.h = 36;
        std::unique_ptr<rtengine::WorkingImageSession, decltype(&rtengine::destroyWorkingImageSession)>
            session(create(down), rtengine::destroyWorkingImageSession);
        check(session && !error, "offset crop downsample create");
        const auto g = rtengine::getWorkingImageSessionGeometry(session.get());
        check(g.retainedWidth == 43 && g.retainedHeight == 32
            && g.resolutionScale == 2.0 / 3.0, "noninteger retained canvas");
        std::unique_ptr<rtengine::IImagefloat> result(
            rtengine::finishWorkingImageSession(session.get(), nullptr, 0, error));
        check(result && !error && result->getWidth() == 32 && result->getHeight() == 24,
              "offset scaled crop finish geometry");
    }
    auto reject = [&](const rtengine::procparams::ProcParams& params) {
        std::unique_ptr<rtengine::WorkingImageSession, decltype(&rtengine::destroyWorkingImageSession)>
            session(create(params), rtengine::destroyWorkingImageSession);
        check(!session && error == -2, "unsupported source pipeline must reject");
    };
    auto edited = p; edited.resize.allowUpscaling = true; reject(edited);
    edited = p; edited.resize.dataspec = 1; edited.resize.width = 128; reject(edited);
    edited = p; edited.rotate.degree = 2; reject(edited);
    edited = p; edited.distortion.amount = 0.1; reject(edited);
    edited = p; edited.perspective.method = "simple"; edited.perspective.horizontal = 10; reject(edited);
    edited = p; edited.framing.enabled = true; reject(edited);
    edited = p; edited.locallab.enabled = true; reject(edited);
    std::cout << "PASS internal source crop/no-upscale and unsupported pipeline fixtures\n";
}
#endif

void check(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("rte-fallback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tiff");
    ~Fixture() { std::error_code ec; std::filesystem::remove(path, ec); }
};
struct Image { rte_image_buffer v{}; ~Image() { rte_image_buffer_free(&v); } };
struct Session { rte_raw_session* v = nullptr; ~Session() { rte_raw_session_close(v); } };
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try {
        Fixture fixture;
        TIFF* tif = TIFFOpen(fixture.path.c_str(), "w");
        check(tif, "TIFFOpen");
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 64);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 48);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        bool written = true;
        std::vector<unsigned char> row(64 * 3);
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 64; ++x) {
                row[x * 3] = x * 3; row[x * 3 + 1] = y * 4; row[x * 3 + 2] = 80;
            }
            written = written && TIFFWriteScanline(tif, row.data(), y, 0) >= 0;
        }
        TIFFClose(tif);
        check(written, "TIFFWriteScanline");
        {
            auto image = Exiv2::ImageFactory::open(fixture.path.string());
            image->readMetadata();
            image->exifData()["Exif.Image.DefaultCropOrigin"] = "2 2";
            image->exifData()["Exif.Image.DefaultCropSize"] = "60 44";
            image->writeMetadata();
        }
        rte_engine_options engine{}; engine.resource_path = argv[1];
        check(rte_engine_init(&engine) == 0, "init");
#ifdef RTE_GEOMETRY_ENGINE_FIXTURES
        processor_fixtures(fixture.path.c_str());
#endif
        rte_decode_options options{}; options.struct_size = sizeof(options); options.scale = RTE_SCALE_2K;
        // Public source open must clear a nonnull sentinel, never silently use V1.
        rte_raw_session* rejected = reinterpret_cast<rte_raw_session*>(1);
        check(rte_raw_source_session_open(fixture.path.c_str(), &options, &rejected) == -2
              && rejected == nullptr, "source fallback must reject/clear handle");
        Image legacy;
        check(rte_decode(fixture.path.c_str(), &options, &legacy.v) == 0, "legacy decode");
        Session v1;
        check(rte_raw_session_open(fixture.path.c_str(), &options, &v1.v) == 0, "V1 open");
        // Delete only the synthetic fixture after open: fallback must be cached.
        check(std::filesystem::remove(fixture.path), "hide fixture");
        for (int i = 0; i < 2; ++i) {
            Image result;
            check(rte_raw_session_finish(v1.v, nullptr, 0, &result.v) == 0, "V1 finish");
            check(result.v.width == legacy.v.width && result.v.height == legacy.v.height
                && result.v.size == legacy.v.size && result.v.data && legacy.v.data
                && !std::memcmp(result.v.data, legacy.v.data, result.v.size), "V1 fallback parity");
        }
        std::cout << "PASS synthetic EXIF fallback rejection / V1 legacy parity / no reread\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
