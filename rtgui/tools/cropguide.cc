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

#include "cropguide.h"

#include "eventmapper.h"
#include "guiutils.h"
#include "multilangmgr.h"
#include "paramsedited.h"
#include "rtimage.h"

#include <array>

namespace {

using CropParams = rtengine::procparams::CropParams;
using CropGuideParams = rtengine::procparams::CropGuideParams;

struct GuideTypeOptions {
    enum Index {
        RULE_OF_THIRDS,
        DIAGONALS,
        HARMONIC_MEANS,
        CROSSHAIR,
        GRID,
        GOLDEN_TRIANGLE_1,
        GOLDEN_TRIANGLE_2,
        EPASSPORT,
        CENTERED_SQUARE
    };
};
// clang-format off
constexpr std::array<const char*, 9> GUIDE_TYPE_OPTIONS = {
    "TP_CROP_GTRULETHIRDS",
    "TP_CROP_GTDIAGONALS",
    "TP_CROP_GTHARMMEANS",
    "TP_CROP_GTCROSSHAIR",
    "TP_CROP_GTGRID",
    "TP_CROP_GTTRIANGLE1",
    "TP_CROP_GTTRIANGLE2",
    "TP_CROP_GTEPASSPORT",
    "TP_CROP_GTCENTEREDSQUARE"
};
// clang-format on

GuideTypeOptions::Index mapGuideType(CropParams::Guide type)
{
    using Guide = CropParams::Guide;
    using Index = GuideTypeOptions::Index;

    switch (type) {
        case Guide::RULE_OF_THIRDS:
            return Index::RULE_OF_THIRDS;
        case Guide::RULE_OF_DIAGONALS:
            return Index::DIAGONALS;
        case Guide::HARMONIC_MEANS:
            return Index::HARMONIC_MEANS;
        case Guide::CROSSHAIR:
            return Index::CROSSHAIR;
        case Guide::GRID:
            return Index::GRID;
        case Guide::GOLDEN_TRIANGLE_1:
            return Index::GOLDEN_TRIANGLE_1;
        case Guide::GOLDEN_TRIANGLE_2:
            return Index::GOLDEN_TRIANGLE_2;
        case Guide::EPASSPORT:
            return Index::EPASSPORT;
        case Guide::CENTERED_SQUARE:
            return Index::CENTERED_SQUARE;
        default:
            return Index::RULE_OF_THIRDS;
    }
}

} // namespace

const Glib::ustring CropGuide::TOOL_NAME = "cropguide";

CropGuide::CropGuide()
    : FoldableToolPanel(this, TOOL_NAME, M("TP_CROP_GUIDE_LABEL"), false, true)
    , m_presets{}
    , m_did_override(false)
{
    setupEvents();
    setupPresets();

    show_all();
}

void CropGuide::setupEvents()
{
    auto m = ProcEventMapper::getInstance();

    EvCropGuideEnabled = m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_ENABLED");
    EvCropGuidePresetChanged = m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_PRESET_CHANGED");
}

void CropGuide::setupPresets()
{
    auto grid = Gtk::manage(new Gtk::Grid());
    grid->set_row_homogeneous(false);
    grid->set_column_homogeneous(false);
    grid->set_hexpand(true);
    grid->set_halign(Gtk::ALIGN_FILL);
    pack_start(*grid, true, true);

    size_t curr_row = 0;
    for (size_t i = 0; i < m_presets.size(); i++, curr_row++) {
        const auto type_index = static_cast<GuideTypeOptions::Index>(i);
        auto& preset = m_presets.at(i);

        preset.visible_icon = std::unique_ptr<RTImage>(
            new RTImage("power-on-small", Gtk::ICON_SIZE_BUTTON));
        preset.hidden_icon = std::unique_ptr<RTImage>(
            new RTImage("power-off-small", Gtk::ICON_SIZE_BUTTON));

        auto visible_button = Gtk::manage(new Gtk::ToggleButton());
        preset.visibility_button = visible_button;
        visible_button->set_image(*preset.hidden_icon);
        visible_button->set_relief(Gtk::RELIEF_NONE);
        preset.visibility_conn = visible_button->signal_toggled().connect(sigc::bind(
                sigc::mem_fun(this, &CropGuide::onPresetToggled), i));
        grid->attach(*visible_button, 0, curr_row);

        auto label = Gtk::manage(new Gtk::Label(M(GUIDE_TYPE_OPTIONS.at(i))));
        label->set_hexpand(true);
        label->set_halign(Gtk::ALIGN_START);
        label->set_line_wrap(true);
        grid->attach(*label, 1, curr_row);

        if (type_index == GuideTypeOptions::Index::GOLDEN_TRIANGLE_1) {
            curr_row++;

            auto button_box = Gtk::manage(new Gtk::Box());
            button_box->set_halign(Gtk::ALIGN_START);
            grid->attach(*button_box, 1, curr_row);

            auto add_button = [&](const char* icon) {
                auto button = Gtk::manage(new Gtk::Button());
                button->set_image(*Gtk::manage(
                        new RTImage(icon, Gtk::ICON_SIZE_BUTTON)));
                button->set_relief(Gtk::RELIEF_NONE);
                button->set_hexpand(false);
                button_box->pack_start(*button);

                return button;
            };

            auto rotate_left_button = add_button("rotate-left-small");
            auto rotate_right_button = add_button("rotate-right-small");
            auto flip_h_button = add_button("flip-horizontal");
            auto flip_v_button = add_button("flip-vertical");
            auto undo_button = add_button("undo-small");

            rotate_left_button->signal_clicked().connect(sigc::bind(
                sigc::mem_fun(this, &CropGuide::onRotateLeft), i));
            rotate_right_button->signal_clicked().connect(sigc::bind(
                sigc::mem_fun(this, &CropGuide::onRotateRight), i));
            flip_h_button->signal_clicked().connect(sigc::bind(
                sigc::mem_fun(this, &CropGuide::onFlipHorizontal), i));
            flip_v_button->signal_clicked().connect(sigc::bind(
                sigc::mem_fun(this, &CropGuide::onFlipVertical), i));
            undo_button->signal_clicked().connect(sigc::bind(
                sigc::mem_fun(this, &CropGuide::onReset), i));
        }
    }

}

void CropGuide::read(const rtengine::procparams::ProcParams* pp,
                     const ParamsEdited* pedited)
{
    if (!pp->cropGuide.override) {
        m_did_override = true;

        for (auto& preset : m_presets) {
            preset.rotate = CropGuideParams::Rotate::BY_0;
            preset.mirror = CropGuideParams::Mirror::AboutAxis::NONE;
            preset.is_rotate_dirty = false;
            preset.is_mirror_dirty = false;

            ConnectionBlocker block(preset.visibility_conn);
            preset.visibility_button->set_active(false);
            preset.visibility_button->set_image(*preset.hidden_icon);
            preset.is_dirty = false;
        }

        if (pp->crop.guide == CropParams::Guide::NONE) {
            setEnabled(false);
        } else if (pp->crop.guide == CropParams::Guide::FRAME) {
            setEnabled(true);
        } else {
            setEnabled(true);

            GuideTypeOptions::Index type = mapGuideType(pp->crop.guide);
            auto& preset = m_presets.at(static_cast<size_t>(type));

            ConnectionBlocker block(preset.visibility_conn);
            preset.visibility_button->set_active(true);
            preset.visibility_button->set_image(*preset.visible_icon);
        }
    } else {
        m_did_override = false;

        setEnabled(pp->cropGuide.enabled);
        for (size_t i = 0; i < m_presets.size(); i++) {
            auto& from = pp->cropGuide.presets.at(i);
            auto& preset = m_presets.at(i);

            if (pedited) {
                auto& from_edited = pedited->cropGuide.presets.at(i);
                preset.is_dirty = from_edited.enabled;
                preset.is_rotate_dirty = from_edited.rotate;
                preset.is_mirror_dirty = from_edited.mirror;
            }

            ConnectionBlocker block(preset.visibility_conn);
            preset.visibility_button->set_active(from.enabled);
            preset.visibility_button->set_image(
                from.enabled ? *preset.visible_icon : *preset.hidden_icon);
        }
    }
}

void CropGuide::write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited)
{
    // Losing a crop guide is a minor inconvenience compared to losing
    // backwards compatibility
    pp->crop.guide = getEnabled() ? CropParams::Guide::FRAME : CropParams::Guide::NONE;
    pp->cropGuide.override = true;
    pp->cropGuide.enabled = getEnabled();

    if (pedited) {
        if (m_did_override) {
            pedited->crop.guide = true;
            pedited->cropGuide.override = true;
        }
        pedited->cropGuide.enabled = !get_inconsistent();
    }

    for (size_t i = 0; i < m_presets.size(); i++) {
        auto& preset = m_presets.at(i);
        auto& to = pp->cropGuide.presets.at(i);

        to.enabled = preset.visibility_button->get_active();

        if (pedited) {
            auto& to_edited = pedited->cropGuide.presets.at(i);
            to_edited.enabled = preset.is_dirty;
            to_edited.rotate = preset.is_rotate_dirty;
            to_edited.mirror = preset.is_mirror_dirty;
        }
    }
}

void CropGuide::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(false);
}

void CropGuide::enabledChanged()
{
    if (!listener) return;

    if (get_inconsistent()) {
        listener->panelChanged(EvCropGuideEnabled, M("GENERAL_UNCHANGED"));
    } else if (getEnabled()) {
        listener->panelChanged(EvCropGuideEnabled, M("GENERAL_ENABLED"));
    } else {
        listener->panelChanged(EvCropGuideEnabled, M("GENERAL_DISABLED"));
    }
}

void CropGuide::onPresetToggled(size_t index)
{
    if (!listener) return;

    auto& preset = m_presets.at(index);
    bool is_visible = preset.visibility_button->get_active();
    preset.visibility_button->set_image(
        is_visible ? *preset.visible_icon : *preset.hidden_icon);
    preset.is_dirty = true;

    listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
}

void CropGuide::onRotateLeft(size_t index)
{
    if (!listener) return;

    using Rotate = CropGuideParams::Rotate;
    auto& preset = m_presets.at(index);
    switch (preset.rotate) {
        case Rotate::BY_0:
            preset.rotate = Rotate::BY_90;
            break;
        case Rotate::BY_90:
            preset.rotate = Rotate::BY_180;
            break;
        case Rotate::BY_180:
            preset.rotate = Rotate::BY_270;
            break;
        case Rotate::BY_270:
            preset.rotate = Rotate::BY_0;
            break;
    }
    preset.is_rotate_dirty = true;

    listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
}

void CropGuide::onRotateRight(size_t index)
{
    if (!listener) return;

    using Rotate = CropGuideParams::Rotate;
    auto& preset = m_presets.at(index);
    switch (preset.rotate) {
        case Rotate::BY_0:
            preset.rotate = Rotate::BY_270;
            break;
        case Rotate::BY_90:
            preset.rotate = Rotate::BY_0;
            break;
        case Rotate::BY_180:
            preset.rotate = Rotate::BY_90;
            break;
        case Rotate::BY_270:
            preset.rotate = Rotate::BY_180;
            break;
    }
    preset.is_rotate_dirty = true;

    listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
}

void CropGuide::onFlipHorizontal(size_t index)
{
    if (!listener) return;

    auto& preset = m_presets.at(index);
    preset.mirror = static_cast<CropGuideParams::Mirror::AboutAxis>(
        preset.mirror ^ CropGuideParams::Mirror::AboutAxis::Y);
    preset.is_mirror_dirty = true;

    listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
}

void CropGuide::onFlipVertical(size_t index)
{
    if (!listener) return;

    auto& preset = m_presets.at(index);
    preset.mirror = static_cast<CropGuideParams::Mirror::AboutAxis>(
        preset.mirror ^ CropGuideParams::Mirror::AboutAxis::X);
    preset.is_mirror_dirty = true;

    listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
}

void CropGuide::onReset(size_t index)
{
    if (!listener) return;

    auto& preset = m_presets.at(index);
    preset.rotate = CropGuideParams::Rotate::BY_0;
    preset.mirror = CropGuideParams::Mirror::NONE;
    preset.is_rotate_dirty = true;
    preset.is_mirror_dirty = true;

    listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
}
