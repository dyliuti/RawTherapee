#include "simpleprocess_geometry.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#ifdef RTE_GEOMETRY_ENGINE_FIXTURES
#include "improcfun.h"
#include "procparams.h"
#endif

#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " #x "\n"; return 1; } } while (false)
int main()
{
    using namespace rtengine::source_preview;
    int checks = 0;
    CHECK(supported(1.0, false, false));
    CHECK(supported(0.25, false, false));
    for (double s : {0.0, -1.0, 1.00001, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) CHECK(!supported(s, false, false));
    for (double s : {0.25, 1.0}) {
        CHECK(!supported(s, true, false));
        CHECK(!supported(s, false, true));
        CHECK(!supported(s, true, true));
    }
    Crop out{};
    CHECK(scaled_crop(6000, 4000, {600, 400, 3000, 2000}, 0.5, 3000, 2000, out));
    CHECK(out.x == 300 && out.y == 200 && out.w == 1500 && out.h == 1000);
    // Portrait canvas is already oriented by getImage (including EXIF rotation).
    CHECK(scaled_crop(4000, 6000, {400, 600, 2000, 3000}, 0.5, 2000, 3000, out));
    CHECK(out.x == 200 && out.y == 300 && out.w == 1000 && out.h == 1500);
    CHECK(scaled_crop(6000, 4000, {5000, 3000, 1000, 1000}, 1.0, 6000, 4000, out));
    CHECK(out.x == 5000 && out.y == 3000 && out.w == 1000 && out.h == 1000);
    CHECK(scaled_crop(6001, 4001, {1, 1, 3001, 2001}, 0.5, 3001, 2001, out));
    CHECK(out.x == 1 && out.y == 1 && out.w == 1501 && out.h == 1001);
    CHECK(!scaled_crop(6000, 4000, {0, 0, 1, 1}, 0.1, 600, 400, out));
    CHECK(out.x == 0 && out.y == 0 && out.w == 0 && out.h == 0);
    CHECK(!scaled_crop(6000, 4000, {-1, 0, 20, 20}, 0.5, 3000, 2000, out));
    CHECK(!scaled_crop(6000, 4000, {5999, 0, 2, 20}, 0.5, 3000, 2000, out));
    CHECK(!scaled_crop(6000, 4000, {1, 0, 5999, 4000}, 0.5, 3000, 2000, out));
    CHECK(!scaled_crop(6000, 4000, {0, 0, 6000, 4000}, 2.0, 12000, 8000, out));
#ifdef RTE_GEOMETRY_ENGINE_FIXTURES
    // Use the actual engine implementation, not a copied resize formula.
    rtengine::procparams::ProcParams p;
    rtengine::ImProcFunctions ipf(&p, false);
    p.resize.enabled = true;
    p.resize.width = p.resize.height = 2000;
    p.resize.allowUpscaling = false; // public open's post-rule override
    p.crop.enabled = true;
    p.crop.x = 5000; p.crop.y = 3000; p.crop.w = p.crop.h = 1000;
    int w = 0, h = 0;
    CHECK(p.resize.dataspec == 3 && p.resize.appliesTo == "Cropped area");
    CHECK(ipf.resizeScale(&p, 6000, 4000, w, h) == 1.0);
    CHECK(w == 1000 && h == 1000); // alleged 2x upscale is NOT reachable
    p.resize.allowUpscaling = true;
    const double upscale = ipf.resizeScale(&p, 6000, 4000, w, h);
    CHECK(upscale == 2.0 && !supported(upscale, false, false));
    p.resize.allowUpscaling = false;
    p.crop.x = 600; p.crop.y = 400; p.crop.w = 3000; p.crop.h = 2000;
    const double down = ipf.resizeScale(&p, 6000, 4000, w, h);
    CHECK(down == 2.0 / 3.0 && w == 2000 && h == 1333);
    CHECK(scaled_crop(6000, 4000, {600, 400, 3000, 2000}, down, 4000, 2667, out));
    CHECK(out.x == 400 && out.y == 267 && out.w == w && out.h == h);
    p.resize.appliesTo = "Full image";
    CHECK(ipf.resizeScale(&p, 6000, 4000, w, h) == 1.0 / 3.0);
    CHECK(w == 1000 && h == 667);
    p.resize.dataspec = 1; p.resize.width = 12000;
    CHECK(!supported(ipf.resizeScale(&p, 6000, 4000, w, h), false, false));
    p.rotate.degree = 2;
    CHECK(!supported(0.5, ipf.needsTransform(6000, 4000, 0, nullptr), false));
    p.rotate.degree = 0;
    p.distortion.amount = 0.1;
    CHECK(!supported(0.5, ipf.needsTransform(6000, 4000, 0, nullptr), false));
    p.distortion.amount = 0;
    p.perspective.method = "simple";
    p.perspective.horizontal = 10;
    CHECK(!supported(0.5, ipf.needsTransform(6000, 4000, 0, nullptr), false));
#endif
    std::cout << "PASS source preview geometry (" << checks << " checks)\n";
}
