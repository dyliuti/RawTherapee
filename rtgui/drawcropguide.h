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

#pragma once

#include <cairomm/context.h>

namespace rtengine {
namespace procparams {
struct CropParams;
}
}

void drawCrop(const Cairo::RefPtr<Cairo::Context>& cr,
              double imx, double imy, double imw, double imh,
              double clipWidth, double clipHeight,
              double startx, double starty, double scale,
              const rtengine::procparams::CropParams& cparams,
              bool drawGuide = true, bool useBgColor = true,
              bool fullImageVisible = true);
