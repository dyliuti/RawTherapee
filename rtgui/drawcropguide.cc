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

namespace
{

void drawCropGuides(const Cairo::RefPtr<Cairo::Context>& cr,
                    double rectx1, double recty1, double rectx2, double recty2,
                    const rtengine::procparams::CropParams& cparams)
{
    if (cparams.guide == rtengine::procparams::CropParams::Guide::NONE) return;

    cr->set_line_width (1.0);
    cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
    cr->move_to (rectx1, recty1);
    cr->line_to (rectx2, recty1);
    cr->line_to (rectx2, recty2);
    cr->line_to (rectx1, recty2);
    cr->line_to (rectx1, recty1);
    cr->stroke ();
    cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
    cr->set_dash (std::valarray<double>({4}), 0);
    cr->move_to (rectx1, recty1);
    cr->line_to (rectx2, recty1);
    cr->line_to (rectx2, recty2);
    cr->line_to (rectx1, recty2);
    cr->line_to (rectx1, recty1);
    cr->stroke ();
    cr->set_dash (std::valarray<double>(), 0);

    if (
        cparams.guide != rtengine::procparams::CropParams::Guide::RULE_OF_DIAGONALS
        && cparams.guide != rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_1
        && cparams.guide != rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2
    ) {
        // draw guide lines
        std::vector<double> horiz_ratios;
        std::vector<double> vert_ratios;

        switch (cparams.guide) {
            case rtengine::procparams::CropParams::Guide::NONE:
            case rtengine::procparams::CropParams::Guide::FRAME:
            case rtengine::procparams::CropParams::Guide::RULE_OF_DIAGONALS:
            case rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_1:
            case rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2: {
                break;
            }

            case rtengine::procparams::CropParams::Guide::RULE_OF_THIRDS: {
                horiz_ratios.push_back (1.0 / 3.0);
                horiz_ratios.push_back (2.0 / 3.0);
                vert_ratios.push_back (1.0 / 3.0);
                vert_ratios.push_back (2.0 / 3.0);
                break;
            }

            case rtengine::procparams::CropParams::Guide::HARMONIC_MEANS: {
                horiz_ratios.push_back (1.0 - 0.618);
                horiz_ratios.push_back (0.618);
                vert_ratios.push_back (0.618);
                vert_ratios.push_back (1.0 - 0.618);
                break;
            }

            case rtengine::procparams::CropParams::Guide::GRID: {
                // To have even distribution, normalize it a bit
                const int longSideNumLines = 10;

                int w = rectx2 - rectx1, h = recty2 - recty1;

                if (w > longSideNumLines && h > longSideNumLines) {
                    if (w > h) {
                        for (int i = 1; i < longSideNumLines; i++) {
                            vert_ratios.push_back ((double)i / longSideNumLines);
                        }

                        int shortSideNumLines = (int)round(h * (double)longSideNumLines / w);

                        for (int i = 1; i < shortSideNumLines; i++) {
                            horiz_ratios.push_back ((double)i / shortSideNumLines);
                        }
                    } else {
                        for (int i = 1; i < longSideNumLines; i++) {
                            horiz_ratios.push_back ((double)i / longSideNumLines);
                        }

                        int shortSideNumLines = (int)round(w * (double)longSideNumLines / h);

                        for (int i = 1; i < shortSideNumLines; i++) {
                            vert_ratios.push_back ((double)i / shortSideNumLines);
                        }
                    }
                }
                break;
            }

            case rtengine::procparams::CropParams::Guide::EPASSPORT: {
                /* Official measurements do not specify exact ratios, just min/max measurements within which the eyes and chin-crown distance must lie. I averaged those measurements to produce these guides.
                 * The first horizontal guide is for the crown, the second is roughly for the nostrils, the third is for the chin.
                 * http://www.homeoffice.gov.uk/agencies-public-bodies/ips/passports/information-photographers/
                 * "(...) the measurement of the face from the bottom of the chin to the crown (ie the top of the head, not the top of the hair) is between 29mm and 34mm."
                 */
                horiz_ratios.push_back (7.0 / 45.0);
                horiz_ratios.push_back (26.0 / 45.0);
                horiz_ratios.push_back (37.0 / 45.0);
                vert_ratios.push_back (0.5);
                break;
            }

            case rtengine::procparams::CropParams::Guide::CENTERED_SQUARE: {
                const double w = rectx2 - rectx1, h = recty2 - recty1;
                double ratio = w / h;
                if (ratio < 1.0) {
                    ratio = 1.0 / ratio;
                    horiz_ratios.push_back((ratio - 1.0) / (2.0 * ratio));
                    horiz_ratios.push_back(1.0 - (ratio - 1.0) / (2.0 * ratio));
                } else {
                    vert_ratios.push_back((ratio - 1.0) / (2.0 * ratio));
                    vert_ratios.push_back(1.0 - (ratio - 1.0) / (2.0 * ratio));
                }
                break;
            }
        }

        // Horizontals
        for (size_t i = 0; i < horiz_ratios.size(); i++) {
            cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
            cr->move_to (rectx1, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->line_to (rectx2, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->stroke ();
            cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
            std::valarray<double> ds (1);
            ds[0] = 4;
            cr->set_dash (ds, 0);
            cr->move_to (rectx1, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->line_to (rectx2, recty1 + round((recty2 - recty1) * horiz_ratios[i]));
            cr->stroke ();
            ds.resize (0);
            cr->set_dash (ds, 0);
        }

        // Verticals
        for (size_t i = 0; i < vert_ratios.size(); i++) {
            cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
            cr->move_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty1);
            cr->line_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty2);
            cr->stroke ();
            cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
            std::valarray<double> ds (1);
            ds[0] = 4;
            cr->set_dash (ds, 0);
            cr->move_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty1);
            cr->line_to (rectx1 + round((rectx2 - rectx1) * vert_ratios[i]), recty2);
            cr->stroke ();
            ds.resize (0);
            cr->set_dash (ds, 0);
        }
    } else if (cparams.guide == rtengine::procparams::CropParams::Guide::RULE_OF_DIAGONALS) {
        double corners_from[4][2];
        double corners_to[4][2];
        int mindim = std::min(rectx2 - rectx1, recty2 - recty1);
        corners_from[0][0] = rectx1;
        corners_from[0][1] = recty1;
        corners_to[0][0]   = rectx1 + mindim;
        corners_to[0][1]   = recty1 + mindim;
        corners_from[1][0] = rectx1;
        corners_from[1][1] = recty2;
        corners_to[1][0]   = rectx1 + mindim;
        corners_to[1][1]   = recty2 - mindim;
        corners_from[2][0] = rectx2;
        corners_from[2][1] = recty1;
        corners_to[2][0]   = rectx2 - mindim;
        corners_to[2][1]   = recty1 + mindim;
        corners_from[3][0] = rectx2;
        corners_from[3][1] = recty2;
        corners_to[3][0]   = rectx2 - mindim;
        corners_to[3][1]   = recty2 - mindim;

        for (int i = 0; i < 4; i++) {
            cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
            cr->move_to (corners_from[i][0], corners_from[i][1]);
            cr->line_to (corners_to[i][0], corners_to[i][1]);
            cr->stroke ();
            cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
            std::valarray<double> ds (1);
            ds[0] = 4;
            cr->set_dash (ds, 0);
            cr->move_to (corners_from[i][0], corners_from[i][1]);
            cr->line_to (corners_to[i][0], corners_to[i][1]);
            cr->stroke ();
            ds.resize (0);
            cr->set_dash (ds, 0);
        }
    } else if (
        cparams.guide == rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_1
        || cparams.guide == rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2
    ) {
        // main diagonal
        if(cparams.guide == rtengine::procparams::CropParams::Guide::GOLDEN_TRIANGLE_2) {
            std::swap(rectx1, rectx2);
        }

        cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
        cr->move_to (rectx1, recty1);
        cr->line_to (rectx2, recty2);
        cr->stroke ();
        cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
        cr->set_dash (std::valarray<double>({4}), 0);
        cr->move_to (rectx1, recty1);
        cr->line_to (rectx2, recty2);
        cr->stroke ();
        cr->set_dash (std::valarray<double>(), 0);

        double height = recty2 - recty1;
        double width = rectx2 - rectx1;
        double d = sqrt(height * height + width * width);
        double alpha = asin(width / d);
        double beta = asin(height / d);
        double a = sin(beta) * height;
        double b = sin(alpha) * height;

        double x = (a * b) / height;
        double y = height - (b * (d - a)) / width;
        cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
        cr->move_to (rectx1, recty2);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
        cr->set_dash (std::valarray<double>({4}), 0);
        cr->move_to (rectx1, recty2);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_dash (std::valarray<double>(), 0);

        x = width - (a * b) / height;
        y = (b * (d - a)) / width;
        cr->set_source_rgba (1.0, 1.0, 1.0, 0.618);
        cr->move_to (rectx2, recty1);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_source_rgba (0.0, 0.0, 0.0, 0.618);
        cr->set_dash (std::valarray<double>({4}), 0);
        cr->move_to (rectx2, recty1);
        cr->line_to (rectx1 + x, recty1 + y);
        cr->stroke ();
        cr->set_dash (std::valarray<double>(), 0);
    }
}

}  // namespace

void drawCrop(const Cairo::RefPtr<Cairo::Context>& cr,
              double imx, double imy, double imw, double imh,
              double clipWidth, double clipHeight,
              double startx, double starty, double scale,
              const rtengine::procparams::CropParams& cparams,
              bool drawGuide, bool useBgColor, bool fullImageVisible)
{
    cr->save();

    cr->set_line_width(0.0);
    cr->rectangle(imx, imy, clipWidth, clipHeight);
    cr->clip();

    double c1x = (cparams.x - startx) * scale;
    double c1y = (cparams.y - starty) * scale;
    double c2x = (cparams.x + cparams.w - startx) * scale - (fullImageVisible ? 0.0 : 1.0);
    double c2y = (cparams.y + cparams.h - starty) * scale - (fullImageVisible ? 0.0 : 1.0);

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

    cr->rectangle (imx, imy, imw + 0.5, round(c1y) + 0.5);
    cr->rectangle (imx, round(imy + c2y) + 0.5, imw + 0.5, round(imh - c2y) + 0.5);
    cr->rectangle (imx, round(imy + c1y) + 0.5, round(c1x) + 0.5, round(c2y - c1y + 1) + 0.5);
    cr->rectangle (round(imx + c2x) + 0.5, round(imy + c1y) + 0.5, round(imw - c2x) + 0.5, round(c2y - c1y + 1) + 0.5);
    cr->fill ();

    cr->restore();

    // rectangle around the cropped area and guides
    if (cparams.guide != rtengine::procparams::CropParams::Guide::NONE && drawGuide) {
        double rectx1 = round(c1x) + imx + 0.5;
        double recty1 = round(c1y) + imy + 0.5;
        double rectx2 = round(c2x) + imx + 0.5;
        double recty2 = round(c2y) + imy + 0.5;

        if(fullImageVisible) {
            rectx2 = std::min(rectx2, imx + imw - 0.5);
            recty2 = std::min(recty2, imy + imh - 0.5);
        }

        drawCropGuides(cr, rectx1, recty1, rectx2, recty2, cparams);
    }
}
