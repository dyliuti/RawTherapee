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

// clang-format off
constexpr std::array<const char*, 9> GUIDE_TYPE_OPTIONS = {
    "TP_CROP_GTRULETHIRDS",
    "TP_CROP_GTDIAGONALS",
    "TP_CROP_GTHARMMEANS",
    "TP_CROP_GTCROSSHAIR",
    "TP_CROP_GTGRID",
    "TP_CROP_GTTRIANGLE",
    "TP_CROP_GTGOLDENRATIO",
    "TP_CROP_GTEPASSPORT",
    "TP_CROP_GTCENTEREDSQUARE"
};
// clang-format on

} // namespace

const Glib::ustring CropGuide::TOOL_NAME = "cropguide";

CropGuide::CropGuide()
    : FoldableToolPanel(this, TOOL_NAME, M("TP_CROP_GUIDE_LABEL"), false, true)
    , m_presets{}
    , m_mirror_golden_triangle(false)
    , m_dirty_mirror_golden_triangle(false)
    , m_rotate_golden_ratio(false)
    , m_dirty_rotate_golden_ratio(false)
    , m_mirror_golden_ratio(false)
    , m_dirty_mirror_golden_ratio(false)
{
    setupEvents();
    setupPresets();
    pack_start(*Gtk::manage(new Gtk::Separator()));
    m_bleed = Gtk::manage(new Adjuster(M("TP_CROP_GUIDE_BLEED"), 0, 10, 1, 0));
    m_bleed->setAdjusterListener(this);
    pack_start(*m_bleed);

    show_all();
}

void CropGuide::setupEvents()
{
    auto m = ProcEventMapper::getInstance();

    EvCropGuideEnabled = m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_ENABLED");
    EvCropGuidePresetChanged = m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_PRESET_CHANGED");
    EvCropGuideBleedChanged = m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_BLEED_CHANGED");
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
        const auto type_index = static_cast<CropGuideParams::PresetIndex>(i);
        auto& preset = m_presets[i];

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

        auto add_button = [&](const char* icon, Gtk::Box* button_box) {
            auto button = Gtk::manage(new Gtk::Button());
            button->set_image(*Gtk::manage(
                    new RTImage(icon, Gtk::ICON_SIZE_BUTTON)));
            button->set_relief(Gtk::RELIEF_NONE);
            button->set_hexpand(false);
            button_box->pack_start(*button);

            return button;
        };

        if (type_index == CropGuideParams::PresetIndex::GOLDEN_TRIANGLE) {
            curr_row++;

            auto button_box = Gtk::manage(new Gtk::Box());
            button_box->set_halign(Gtk::ALIGN_START);
            grid->attach(*button_box, 1, curr_row);

            auto mirror_button = add_button("flip-horizontal", button_box);
            auto undo_button = add_button("undo-small", button_box);

            mirror_button->signal_clicked().connect(
                sigc::mem_fun(this, &CropGuide::onGoldenTriangleMirror));
            undo_button->signal_clicked().connect(
                sigc::mem_fun(this, &CropGuide::onGoldenTriangleReset));
        } else if (type_index == CropGuideParams::PresetIndex::GOLDEN_RATIO) {
            curr_row++;

            auto button_box = Gtk::manage(new Gtk::Box());
            button_box->set_halign(Gtk::ALIGN_START);
            grid->attach(*button_box, 1, curr_row);

            auto rotate_button = add_button("rotate-right-small", button_box);
            auto mirror_button = add_button("flip-horizontal", button_box);
            auto undo_button = add_button("undo-small", button_box);

            rotate_button->signal_clicked().connect(
                sigc::mem_fun(this, &CropGuide::onGoldenRatioRotate));
            mirror_button->signal_clicked().connect(
                sigc::mem_fun(this, &CropGuide::onGoldenRatioMirror));
            undo_button->signal_clicked().connect(
                sigc::mem_fun(this, &CropGuide::onGoldenRatioReset));
        }
    }
}

void CropGuide::read(const rtengine::procparams::ProcParams* pp,
                     const ParamsEdited* pedited)
{
    DisableListener disable_listener(this);
    BlockAdjusterEvents blockBleed(m_bleed);

    setEnabled(pp->cropGuide.enabled);
    m_mirror_golden_triangle = pp->cropGuide.mirror_golden_triangle;
    m_rotate_golden_ratio = pp->cropGuide.rotate_golden_ratio;
    m_mirror_golden_ratio = pp->cropGuide.mirror_golden_ratio;
    m_bleed->setValue(pp->cropGuide.bleed);

    if (pedited) {
        set_inconsistent(multiImage && pedited->cropGuide.enabled);
        m_dirty_mirror_golden_triangle = pedited->cropGuide.mirror_golden_triangle;
        m_dirty_rotate_golden_ratio = pedited->cropGuide.rotate_golden_ratio;
        m_dirty_mirror_golden_ratio = pedited->cropGuide.mirror_golden_ratio;
        m_bleed->setEditedState(pedited->cropGuide.bleed ? Edited : UnEdited);
    };

    for (size_t i = 0; i < m_presets.size(); i++) {
        auto& preset = m_presets[i];
        bool is_enabled = pp->cropGuide.presets[i];

        ConnectionBlocker block(preset.visibility_conn);
        preset.visibility_button->set_active(is_enabled);
        preset.visibility_button->set_image(
            is_enabled ? *preset.visible_icon : *preset.hidden_icon);

        if (pedited) {
            preset.is_dirty = pedited->cropGuide.presets[i];
        }
    }
}

void CropGuide::write(rtengine::procparams::ProcParams* pp, ParamsEdited* pedited)
{
    pp->cropGuide.enabled = getEnabled();
    pp->cropGuide.mirror_golden_triangle = m_mirror_golden_triangle;
    pp->cropGuide.rotate_golden_ratio = m_rotate_golden_ratio;
    pp->cropGuide.mirror_golden_ratio = m_mirror_golden_ratio;
    pp->cropGuide.bleed = m_bleed->getIntValue();

    if (pedited) {
        pedited->cropGuide.enabled = !get_inconsistent();
        pedited->cropGuide.mirror_golden_triangle = m_dirty_mirror_golden_triangle;
        pedited->cropGuide.rotate_golden_ratio = m_dirty_rotate_golden_ratio;
        pedited->cropGuide.mirror_golden_ratio = m_dirty_mirror_golden_ratio;
        pedited->cropGuide.bleed = m_bleed->getEditedState();
    }

    for (size_t i = 0; i < m_presets.size(); i++) {
        auto& preset = m_presets[i];

        pp->cropGuide.presets[i] = preset.visibility_button->get_active();

        if (pedited) {
            pedited->cropGuide.presets[i] = preset.is_dirty;
        }
    }
}

void CropGuide::trimValues(rtengine::procparams::ProcParams* pp)
{
    m_bleed->trimValue(pp->cropGuide.bleed);
}

void CropGuide::setBatchMode(bool batchMode)
{
    ToolPanel::setBatchMode(false);
}

void CropGuide::setAdjusterBehavior(bool bleed)
{
    m_bleed->setAddMode(bleed);
}

void CropGuide::adjusterChanged(Adjuster* adj, double newVal)
{
    if (listener && (getEnabled() || batchMode)) {
        if (adj == m_bleed) {
            listener->panelChanged(EvCropGuideBleedChanged,
                                   Glib::ustring::format(adj->getIntValue()));
        }
    }
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
    auto& preset = m_presets.at(index);
    bool is_visible = preset.visibility_button->get_active();
    preset.visibility_button->set_image(
        is_visible ? *preset.visible_icon : *preset.hidden_icon);
    preset.is_dirty = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
    }
}

void CropGuide::onGoldenTriangleMirror()
{
    m_mirror_golden_triangle = !m_mirror_golden_triangle;
    m_dirty_mirror_golden_triangle = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(
            CropGuideParams::PresetIndex::GOLDEN_TRIANGLE)));
    }
}

void CropGuide::onGoldenTriangleReset()
{
    m_mirror_golden_triangle = false;
    m_dirty_mirror_golden_triangle = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(
            CropGuideParams::PresetIndex::GOLDEN_TRIANGLE)));
    }
}

void CropGuide::onGoldenRatioRotate()
{
    m_rotate_golden_ratio = !m_rotate_golden_ratio;
    m_dirty_rotate_golden_ratio = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(
            CropGuideParams::PresetIndex::GOLDEN_RATIO)));
    }
}

void CropGuide::onGoldenRatioMirror()
{
    m_mirror_golden_ratio = !m_mirror_golden_ratio;
    m_dirty_mirror_golden_ratio = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(
            CropGuideParams::PresetIndex::GOLDEN_RATIO)));
    }
}

void CropGuide::onGoldenRatioReset()
{
    m_mirror_golden_ratio = false;
    m_dirty_mirror_golden_ratio = true;
    m_rotate_golden_ratio = false;
    m_dirty_rotate_golden_ratio = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(
            CropGuideParams::PresetIndex::GOLDEN_RATIO)));
    }
}
