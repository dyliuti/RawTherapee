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

#include "toolpanel.h"

#include "rtengine/procevents.h"
#include "rtengine/procparams.h"

#include <array>
#include <memory>

class MyComboBoxText;
class ParamsEdited;
class RTImage;

namespace Gtk {

class ToggleButton;

} // namespace Gtk

class CropGuide final : public ToolParamBlock, public FoldableToolPanel {
public:
    static const Glib::ustring TOOL_NAME;

    CropGuide();

    void read(const rtengine::procparams::ProcParams* pp,
              const ParamsEdited* pedited = nullptr) override;
    void write(rtengine::procparams::ProcParams* pp,
               ParamsEdited* pedited = nullptr) override;
    void setBatchMode(bool batchMode) override;

    void enabledChanged() override;
    void onPresetToggled(size_t index);
    void onRotateLeft(size_t index);
    void onRotateRight(size_t index);
    void onFlipHorizontal(size_t index);
    void onFlipVertical(size_t index);
    void onReset(size_t index);

private:
    void setupEvents();
    void setupPresets();

    struct Preset {
        std::unique_ptr<RTImage> visible_icon;
        std::unique_ptr<RTImage> hidden_icon;
        Gtk::ToggleButton* visibility_button = nullptr;
        sigc::connection visibility_conn;
        rtengine::procparams::CropGuideParams::Rotate rotate =
            rtengine::procparams::CropGuideParams::Rotate::BY_0;
        rtengine::procparams::CropGuideParams::Mirror::AboutAxis mirror =
            rtengine::procparams::CropGuideParams::Mirror::AboutAxis::NONE;
        bool is_dirty = false;
        bool is_rotate_dirty = false;
        bool is_mirror_dirty = false;
    };

    std::array<Preset, 9> m_presets;

    rtengine::ProcEvent EvCropGuideEnabled;
    rtengine::ProcEvent EvCropGuidePresetChanged;
};
