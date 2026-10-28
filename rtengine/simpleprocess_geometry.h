#pragma once

#include <cmath>
#include <limits>

namespace rtengine {
namespace source_preview {

// Source preview only: legacy fast export deliberately keeps its own policy.
// The requested scale has already been resolved by ImProcFunctions::resizeScale.
inline bool supported(double scale, bool complexGeometry, bool fallbackCrop)
{
    return std::isfinite(scale) && scale > 0.0 && scale <= 1.0
        && !complexGeometry && !fallbackCrop;
}

struct Crop { int x, y, w, h; };

// Map an axis-aligned native crop onto the retained full canvas. Reject a
// collapsed/out-of-bounds rectangle rather than silently disabling the crop.
inline bool scaled_crop(int nativeW, int nativeH, const Crop& crop, double scale,
                        int canvasW, int canvasH, Crop& out)
{
    out = {0, 0, 0, 0};
    if (!supported(scale, false, false) || nativeW <= 0 || nativeH <= 0
        || canvasW <= 0 || canvasH <= 0 || crop.x < 0 || crop.y < 0
        || crop.w <= 0 || crop.h <= 0 || crop.x >= nativeW || crop.y >= nativeH
        || crop.w > nativeW - crop.x || crop.h > nativeH - crop.y) return false;
    const double x = std::round(crop.x * scale), y = std::round(crop.y * scale);
    const double w = std::round(crop.w * scale), h = std::round(crop.h * scale);
    if (x >= canvasW || y >= canvasH || w < 1 || h < 1
        || w > canvasW - x || h > canvasH - y
        || w > std::numeric_limits<int>::max() || h > std::numeric_limits<int>::max()) return false;
    out = {int(x), int(y), int(w), int(h)};
    return true;
}

} // namespace source_preview
} // namespace rtengine
