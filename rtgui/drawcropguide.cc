/*
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2025 Daniel Gao <daniel.gao.work@gmail.com>
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "drawcropguide.h"

#include "options.h"

#include "rtengine/procparams.h"

#include <cmath>

using namespace rtengine::procparams;

namespace
{

using PresetIndex = CropGuideParams::PresetIndex;

constexpr double GUIDE_ALPHA = 0.618;
// constexpr double GOLDEN_RATIO = 1.618;
constexpr double GOLDEN_RATIO_RECIPROCAL = 0.618;  // (1 / GOLDEN_RATIO)

struct CropRect {
    double x0;
    double y0;
    double x1;
    double y1;
};

struct GuideDrawer {
    const Cairo::RefPtr<Cairo::Context>& cr;
    const CropRect& rect;
    const CropGuideParams& params;

    void drawRuleOfThirds();
    void drawHarmonicMeans();
    void drawCrosshair();
    void drawGrid();
    void drawEpassport();
    void drawCenteredSquare();

    void drawDiagonals();
    void drawGoldenTriangle1();
    void drawGoldenTriangle2();

    void drawHorizontal(double ratio);
    void drawVertical(double ratio);

    void drawDashedLine(double x0, double y0, double x1, double y1);
};

void drawFrame(const Cairo::RefPtr<Cairo::Context>& cr,
               const CropRect& rect)
{
    cr->set_line_width(1.0);
    cr->set_source_rgba(1.0, 1.0, 1.0, GUIDE_ALPHA);
    cr->move_to(rect.x0, rect.y0);
    cr->line_to(rect.x1, rect.y0);
    cr->line_to(rect.x1, rect.y1);
    cr->line_to(rect.x0, rect.y1);
    cr->line_to(rect.x0, rect.y0);
    cr->stroke();

    cr->set_source_rgba(0.0, 0.0, 0.0, GUIDE_ALPHA);
    cr->set_dash(std::valarray<double>({4}), 0);
    cr->move_to(rect.x0, rect.y0);
    cr->line_to(rect.x1, rect.y0);
    cr->line_to(rect.x1, rect.y1);
    cr->line_to(rect.x0, rect.y1);
    cr->line_to(rect.x0, rect.y0);
    cr->stroke();
    cr->set_dash(std::valarray<double>(), 0);
}

void drawCropGuides(const Cairo::RefPtr<Cairo::Context>& cr,
                    const CropRect& rect,
                    const CropGuideParams& params,
                    CropGuideOverride override)
{
    switch (override) {
        case CropGuideOverride::FRAME:
            break;
        case CropGuideOverride::DONT_TOUCH:
            if (!params.enabled) {
                return; // Skip drawing guides
            } else {
                break;
            }
        case CropGuideOverride::NO_GUIDES:
        default:
            return; // Skip drawing guides
    }

    drawFrame(cr, rect);

    if (override == CropGuideOverride::FRAME) return;

    GuideDrawer util{cr, rect, params};

    // Horizontal/vertical lines only
    util.drawRuleOfThirds();
    util.drawHarmonicMeans();
    util.drawCrosshair();
    util.drawGrid();
    util.drawEpassport();
    util.drawCenteredSquare();

    // Diagonals
    util.drawDiagonals();
    util.drawGoldenTriangle1();
    util.drawGoldenTriangle2();
}

}  // namespace

void GuideDrawer::drawRuleOfThirds()
{
    if (!params.presets.at(PresetIndex::RULE_OF_THIRDS).enabled) return;

    drawHorizontal(1.0 / 3.0);
    drawHorizontal(2.0 / 3.0);
    drawVertical(1.0 / 3.0);
    drawVertical(2.0 / 3.0);
}

void GuideDrawer::drawHarmonicMeans()
{
    if (!params.presets.at(PresetIndex::HARMONIC_MEANS).enabled) return;

    drawHorizontal(GOLDEN_RATIO_RECIPROCAL);
    drawHorizontal(1.0 - GOLDEN_RATIO_RECIPROCAL);
    drawVertical(GOLDEN_RATIO_RECIPROCAL);
    drawVertical(1.0 - GOLDEN_RATIO_RECIPROCAL);
}

void GuideDrawer::drawCrosshair()
{
    if (!params.presets.at(PresetIndex::CROSSHAIR).enabled) return;

    drawHorizontal(0.5);
    drawVertical(0.5);
}

void GuideDrawer::drawGrid()
{
    if (!params.presets.at(PresetIndex::GRID).enabled) return;

    // To have even distribution, normalize it a bit
    const int longSideNumLines = 10;

    const double w = rect.x1 - rect.x0;
    const double h = rect.y1 - rect.y0;

    if (w > longSideNumLines && h > longSideNumLines) {
        if (w > h) {
            for (int i = 1; i < longSideNumLines; i++) {
                drawVertical(static_cast<double>(i) / longSideNumLines);
            }

            int shortSideNumLines = static_cast<int>(std::round(h * longSideNumLines / w));
            for (int i = 1; i < shortSideNumLines; i++) {
                drawHorizontal(static_cast<double>(i) / shortSideNumLines);
            }
        } else {
            for (int i = 1; i < longSideNumLines; i++) {
                drawHorizontal(static_cast<double>(i) / longSideNumLines);
            }

            int shortSideNumLines = static_cast<int>(std::round(w * longSideNumLines / h));
            for (int i = 1; i < shortSideNumLines; i++) {
                drawVertical(static_cast<double>(i) / shortSideNumLines);
            }
        }
    }
}

/**
 * Official measurements do not specify exact ratios, just min/max measurements
 * within which the eyes and chin-crown distance must lie. I averaged those
 * measurements to produce these guides.
 *
 * The first horizontal guide is for the crown, the second is roughly for the
 * nostrils, the third is for the chin.
 *
 * http://www.homeoffice.gov.uk/agencies-public-bodies/ips/passports/information-photographers/
 * "(...) the measurement of the face from the bottom of the chin to the crown
 * (ie the top of the head, not the top of the hair) is between 29mm and 34mm."
 */
void GuideDrawer::drawEpassport()
{
    if (!params.presets.at(PresetIndex::EPASSPORT).enabled) return;

    drawHorizontal(7.0 / 45.0);
    drawHorizontal(26.0 / 45.0);
    drawHorizontal(37.0 / 45.0);
    drawVertical(0.5);
}

void GuideDrawer::drawCenteredSquare()
{
    if (!params.presets.at(PresetIndex::CENTERED_SQUARE).enabled) return;

    const double w = rect.x1 - rect.x0;
    const double h = rect.y1 - rect.y0;
    double ratio = w / h;

    if (ratio < 1.0) {
        ratio = 1.0 / ratio;
        const double pos = (ratio - 1.0) / (2.0 * ratio);
        drawHorizontal(pos);
        drawHorizontal(1.0 - pos);
    } else {
        const double pos = (ratio - 1.0) / (2.0 * ratio);
        drawVertical(pos);
        drawVertical(1.0 - pos);
    }
}

void GuideDrawer::drawDiagonals()
{
    if (!params.presets.at(PresetIndex::RULE_OF_DIAGONALS).enabled) return;

    double corners_from[4][2];
    double corners_to[4][2];

    double mindim = std::min(rect.x1 - rect.x0, rect.y1 - rect.y0);

    // clang-format off
    corners_from[0][0] = rect.x0;
    corners_from[0][1] = rect.y0;
    corners_to[0][0]   = rect.x0 + mindim;
    corners_to[0][1]   = rect.y0 + mindim;
    corners_from[1][0] = rect.x0;
    corners_from[1][1] = rect.y1;
    corners_to[1][0]   = rect.x0 + mindim;
    corners_to[1][1]   = rect.y1 - mindim;
    corners_from[2][0] = rect.x1;
    corners_from[2][1] = rect.y0;
    corners_to[2][0]   = rect.x1 - mindim;
    corners_to[2][1]   = rect.y0 + mindim;
    corners_from[3][0] = rect.x1;
    corners_from[3][1] = rect.y1;
    corners_to[3][0]   = rect.x1 - mindim;
    corners_to[3][1]   = rect.y1 - mindim;
    // clang-format on

    for (int i = 0; i < 4; i++) {
        drawDashedLine(corners_from[i][0], corners_from[i][1],
                       corners_to[i][0], corners_to[i][1]);
    }
}

void GuideDrawer::drawGoldenTriangle1()
{
    if (!params.presets.at(PresetIndex::GOLDEN_TRIANGLE_1).enabled) return;

    const double x0 = rect.x0;
    const double x1 = rect.x1;

    drawDashedLine(x0, rect.y0, x1, rect.y1);

    const double height = rect.y1 - rect.y0;
    const double width = x1 - x0;
    const double d = std::sqrt(height * height + width * width);
    const double alpha = std::asin(width / d);
    const double beta = std::asin(height / d);
    const double a = std::sin(beta) * height;
    const double b = std::sin(alpha) * height;

    double x = (a * b) / height;
    double y = height - (b * (d - a)) / width;
    drawDashedLine(x0, rect.y1, x0 + x, rect.y0 + y);

    x = width - (a * b) / height;
    y = (b * (d - a)) / width;
    drawDashedLine(x1, rect.y0, x0 + x, rect.y0 + y);
}

void GuideDrawer::drawGoldenTriangle2()
{
    if (!params.presets.at(PresetIndex::GOLDEN_TRIANGLE_2).enabled) return;

    // Swapped
    const double x0 = rect.x1;
    const double x1 = rect.x0;

    drawDashedLine(x0, rect.y0, x1, rect.y1);

    const double height = rect.y1 - rect.y0;
    const double width = x1 - x0;
    const double d = std::sqrt(height * height + width * width);
    const double alpha = std::asin(width / d);
    const double beta = std::asin(height / d);
    const double a = std::sin(beta) * height;
    const double b = std::sin(alpha) * height;

    double x = (a * b) / height;
    double y = height - (b * (d - a)) / width;
    drawDashedLine(x0, rect.y1, x0 + x, rect.y0 + y);

    x = width - (a * b) / height;
    y = (b * (d - a)) / width;
    drawDashedLine(x1, rect.y0, x0 + x, rect.y0 + y);
}

void GuideDrawer::drawHorizontal(double ratio)
{
    double y = rect.y0 + std::round((rect.y1 - rect.y0) * ratio);
    drawDashedLine(rect.x0, y, rect.x1, y);
}

void GuideDrawer::drawVertical(double ratio)
{
    double x = rect.x0 + std::round((rect.x1 - rect.x0) * ratio);
    drawDashedLine(x, rect.y0, x, rect.y1);
}

void GuideDrawer::drawDashedLine(double x0, double y0, double x1, double y1)
{
    cr->set_source_rgba(1.0, 1.0, 1.0, GUIDE_ALPHA);
    cr->move_to(x0, y0);
    cr->line_to(x1, y1);
    cr->stroke();

    cr->set_source_rgba(0.0, 0.0, 0.0, GUIDE_ALPHA);
    cr->set_dash(std::valarray<double>({4}), 0);
    cr->move_to(x0, y0);
    cr->line_to(x1, y1);
    cr->stroke();

    // Reset state
    cr->set_dash(std::valarray<double>(), 0);
}

void drawCrop(const Cairo::RefPtr<Cairo::Context>& cr,
              double imx, double imy, double imw, double imh,
              double clipWidth, double clipHeight,
              double startx, double starty, double scale,
              const CropParams& cropParams,
              const CropGuideParams& cropGuideParams,
              CropGuideOverride cropGuideOverride,
              bool useBgColor, bool fullImageVisible)
{
    cr->save();

    cr->set_line_width(0.0);
    cr->rectangle(imx, imy, clipWidth, clipHeight);
    cr->clip();

    double c1x = (cropParams.x - startx) * scale;
    double c1y = (cropParams.y - starty) * scale;
    double c2x = (cropParams.x + cropParams.w - startx) * scale - (fullImageVisible ? 0.0 : 1.0);
    double c2y = (cropParams.y + cropParams.h - starty) * scale - (fullImageVisible ? 0.0 : 1.0);

    // crop overlay color, linked with crop windows background
    if (options.bgcolor == 0 || !useBgColor) {
        cr->set_source_rgba (options.cutOverlayBrush[0], options.cutOverlayBrush[1], options.cutOverlayBrush[2], options.cutOverlayBrush[3]);
    } else if (options.bgcolor == 1) {
        cr->set_source_rgb (0, 0, 0);
    } else if (options.bgcolor == 2) {
        cr->set_source_rgb (1, 1, 1);
    } else if (options.bgcolor == 3) {
        cr->set_source_rgb (0.467, 0.467, 0.467);
    }

    cr->rectangle(imx, imy, imw + 0.5, std::round(c1y) + 0.5);
    cr->rectangle(imx, std::round(imy + c2y) + 0.5,
                  imw + 0.5, std::round(imh - c2y) + 0.5);
    cr->rectangle(imx, std::round(imy + c1y) + 0.5,
                  std::round(c1x) + 0.5, std::round(c2y - c1y + 1) + 0.5);
    cr->rectangle(std::round(imx + c2x) + 0.5, std::round(imy + c1y) + 0.5,
                  std::round(imw - c2x) + 0.5, std::round(c2y - c1y + 1) + 0.5);
    cr->fill();

    cr->restore();

    if (cropGuideOverride == CropGuideOverride::NO_GUIDES) return;

    // Rectangle around the cropped area for drawing crop guide frame
    CropRect rect {
        /*.x0 =*/ std::round(c1x) + imx + 0.5,
        /*.y0 =*/ std::round(c1y) + imy + 0.5,
        /*.x1 =*/ std::round(c2x) + imx + 0.5,
        /*.y1 =*/ std::round(c2y) + imy + 0.5
    };

    if (fullImageVisible) {
        rect.x1 = std::min(rect.x1, imx + imw - 0.5);
        rect.y1 = std::min(rect.y1, imy + imh - 0.5);
    }
    drawCropGuides(cr, rect, cropGuideParams, cropGuideOverride);
}
