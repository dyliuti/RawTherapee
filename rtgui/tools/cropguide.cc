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
#include "widgets/basic/colorpreview.h"

#include <algorithm>

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

void updateImage(Gtk::ToggleButton* button, bool is_enabled)
{
    if (is_enabled) {
        button->set_image(*Gtk::manage(
            new RTImage("power-on-small", Gtk::ICON_SIZE_BUTTON)));
    } else {
        button->set_image(*Gtk::manage(
            new RTImage("power-off-small", Gtk::ICON_SIZE_BUTTON)));
    }
}

} // namespace

const Glib::ustring CropGuide::TOOL_NAME = "cropguide";

CropGuide::CropGuide()
    : FoldableToolPanel(this, TOOL_NAME, M("TP_CROP_GUIDE_LABEL"), false, true)
    , m_aspect_ratio_store(Gio::ListStore<AspectRatioModel>::create())
    , m_presets{}
    , m_mirror_golden_triangle(false)
    , m_dirty_mirror_golden_triangle(false)
    , m_rotate_golden_ratio(false)
    , m_dirty_rotate_golden_ratio(false)
    , m_mirror_golden_ratio(false)
    , m_dirty_mirror_golden_ratio(false)
    , m_dirty_aspect_ratios(false)
{
    setupEvents();

    setEnabledTooltipMarkup(M("TP_CROP_GUIDE_TOOLTIP"));

    setupPresets();
    pack_start(*Gtk::manage(new Gtk::Separator()));
    setupAspectRatioGuides();

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
    EvCropGuidePresetChanged =
        m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_PRESET_CHANGED");
    EvCropGuideAspectRatioPresetChanged =
        m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_ASPECT_RATIO_PRESET_CHANGED");
    EvCropGuideBleedChanged =
        m->newEvent(MINUPDATE, "HISTORY_MSG_CROP_GUIDE_BLEED_CHANGED");
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

        auto visible_button = Gtk::manage(new Gtk::ToggleButton());
        preset.visibility_button = visible_button;
        updateImage(visible_button, false);
        visible_button->set_relief(Gtk::RELIEF_NONE);
        preset.visibility_conn = visible_button->signal_button_release_event().connect(
            sigc::bind(sigc::mem_fun(*this, &CropGuide::onPresetToggled), i));

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

void CropGuide::setupAspectRatioGuides()
{
    auto box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL));
    auto label = Gtk::manage(new Gtk::Label(M("TP_CROP_GUIDE_ASPECT_RATIO")));
    label->set_halign(Gtk::ALIGN_START);
    box->pack_start(*label);
    m_available_aspect_ratios_combo = Gtk::manage(new MyComboBoxText());
    m_available_aspect_ratios_conn = m_available_aspect_ratios_combo->signal_changed()
        .connect(sigc::mem_fun(this, &CropGuide::onAspectRatioComboChanged));
    box->pack_start(*m_available_aspect_ratios_combo);
    pack_start(*box);

    {
        std::vector<AspectRatio> ratios = getAspectRatios();

        m_aspect_ratio_presets.reserve(ratios.size());
        size_t index = 0;
        for (auto& r : ratios) {
            m_available_aspect_ratios_combo->append(r.label);

            Glib::RefPtr<AspectRatioModel> preset = AspectRatioModel::create();
            preset->aspect_ratio = std::move(r);
            preset->index = index++;
            m_aspect_ratio_presets.push_back(preset);
        }
    }

    m_aspect_ratio_listbox = Gtk::manage(new Gtk::ListBox());
    m_aspect_ratio_listbox->set_selection_mode(Gtk::SELECTION_NONE);

    auto placeholder = Gtk::manage(new Gtk::Label(
        M("TP_CROP_GUIDE_NO_ASPECT_RATIO_GUIDES")));
    placeholder->show();
    m_aspect_ratio_listbox->set_placeholder(*placeholder);

    m_aspect_ratio_listbox->bind_list_store(
        m_aspect_ratio_store,
        sigc::mem_fun(this, &CropGuide::createAspectRatioModelControls));
    pack_start(*m_aspect_ratio_listbox);
}

Gtk::Widget* CropGuide::createAspectRatioModelControls(
    const Glib::RefPtr<AspectRatioModel>& item)
{
    auto grid = Gtk::manage(new Gtk::Grid());

    auto visible_button = Gtk::manage(new Gtk::ToggleButton());
    visible_button->set_active(item->visible);
    updateImage(visible_button, item->visible);
    visible_button->set_relief(Gtk::RELIEF_NONE);
    grid->attach(*visible_button, 0, 0);

    visible_button->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(this, &CropGuide::onAspectRatioPresetToggled),
        visible_button, item->index));

    auto label = Gtk::manage(new Gtk::Label(item->aspect_ratio.label));
    label->set_halign(Gtk::ALIGN_START);
    label->set_line_wrap(true);
    grid->attach(*label, 1, 0);

    auto color_preview = Gtk::manage(new ColorPreview());
    color_preview->set_hexpand(false);
    color_preview->set_halign(Gtk::ALIGN_CENTER);
    color_preview->set_vexpand(false);
    color_preview->set_valign(Gtk::ALIGN_CENTER);
    color_preview->setRgb(item->color.get_red(), item->color.get_green(),
                          item->color.get_blue());
    grid->attach(*color_preview, 0, 1);

    auto box = Gtk::manage(new Gtk::Box());

    auto color_button = Gtk::manage(new Gtk::Button());
    color_button->set_image(*Gtk::manage(
        new RTImage("color-picker-small", Gtk::ICON_SIZE_BUTTON)));
    color_button->set_relief(Gtk::RELIEF_NONE);
    box->pack_start(*color_button, false, false);

    color_button->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(this, &CropGuide::onAspectRatioPresetPickColor),
        item->index,
        color_preview));

    auto remove_button = Gtk::manage(new Gtk::Button());
    remove_button->set_image(*Gtk::manage(
        new RTImage("cancel-small", Gtk::ICON_SIZE_BUTTON)));
    remove_button->set_relief(Gtk::RELIEF_NONE);
    box->pack_start(*remove_button, false, false);

    remove_button->signal_clicked().connect(sigc::bind(
        sigc::mem_fun(this, &CropGuide::onAspectRatioPresetRemoved),
        item->index));

    grid->attach(*box, 1, 1);
    grid->show_all();
    return grid;
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
        updateImage(preset.visibility_button, is_enabled);

        if (pedited) {
            preset.is_dirty = pedited->cropGuide.presets[i];
        }
    }

    m_aspect_ratio_store->remove_all();
    for (const auto& model : m_aspect_ratio_presets) {
        model->active = false;
        model->visible = true;

        Gdk::RGBA color;
        color.set_rgba(1.f, 1.f, 1.f);
        model->color = color;
    }

    for (const auto& param : pp->cropGuide.aspect_ratios) {
        auto& preset = m_aspect_ratio_presets.at(param.preset_index);
        preset->active = true;
        preset->visible = param.enabled;

        Gdk::RGBA color;
        color.set_rgba(static_cast<float>(param.red), static_cast<float>(param.green),
                       static_cast<float>(param.blue));
        preset->color = color;

        m_aspect_ratio_store->insert_sorted(
            preset, sigc::mem_fun(this, &CropGuide::compareAspectRatioModels));
    }

    refreshAvailableAspectRatios();

    if (pedited) {
        m_dirty_aspect_ratios = pedited->cropGuide.aspect_ratios;
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

    if (pedited) {
        pedited->cropGuide.aspect_ratios = m_dirty_aspect_ratios;
    }

    pp->cropGuide.aspect_ratios.clear();
    for (const auto& model : m_aspect_ratio_presets) {
        if (!model->active) continue;

        CropGuideParams::AspectRatioParams params(model->index);
        params.enabled = model->visible;
        params.red = model->color.get_red();
        params.green = model->color.get_green();
        params.blue = model->color.get_blue();

        pp->cropGuide.aspect_ratios.push_back(std::move(params));
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

bool CropGuide::onPresetToggled(GdkEventButton* event, size_t index)
{
    if (event->state & GDK_CONTROL_MASK) {
        Preset& preset = m_presets.at(index);
        preset.visibility_button->toggled();
        bool is_visible = preset.visibility_button->get_active();
        updateImage(preset.visibility_button, is_visible);
        preset.is_dirty = true;
    } else {
        for (size_t i = 0; i < m_presets.size(); i++) {
            Preset& preset = m_presets[i];

            if (i == index) {
                preset.visibility_button->toggled();
                bool is_visible = preset.visibility_button->get_active();
                updateImage(preset.visibility_button, is_visible);
                preset.is_dirty = true;
            } else {
                preset.visibility_button->set_active(false);
                updateImage(preset.visibility_button, false);
                preset.is_dirty = true;
            }
        }
    }

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuidePresetChanged, M(GUIDE_TYPE_OPTIONS.at(index)));
    }

    return true;
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

void CropGuide::onAspectRatioComboChanged()
{
    ConnectionBlocker block(m_available_aspect_ratios_conn);

    int active_row = m_available_aspect_ratios_combo->get_active_row_number();
    Glib::ustring active_text = m_available_aspect_ratios_combo->get_active_text();
    m_available_aspect_ratios_combo->unset_active();
    m_available_aspect_ratios_combo->remove_text(active_row);

    auto is_matching_preset = [&](const Glib::RefPtr<AspectRatioModel>& preset) {
        return active_text == preset->aspect_ratio.label;
    };

    auto it = std::find_if(m_aspect_ratio_presets.begin(), m_aspect_ratio_presets.end(),
                           is_matching_preset);
    if (it == m_aspect_ratio_presets.end()) return;

    Glib::RefPtr<AspectRatioModel> model = *it;
    model->active = true;
    model->visible = true;
    m_dirty_aspect_ratios = true;

    m_aspect_ratio_store->insert_sorted(
        model, sigc::mem_fun(this, &CropGuide::compareAspectRatioModels));

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuideAspectRatioPresetChanged,
                               model->aspect_ratio.label);
    }
}

void CropGuide::onAspectRatioPresetToggled(Gtk::ToggleButton* button, size_t index)
{
    bool is_visible = button->get_active();
    updateImage(button, is_visible);

    auto& preset = m_aspect_ratio_presets.at(index);
    preset->visible = is_visible;
    m_dirty_aspect_ratios = true;

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuideAspectRatioPresetChanged,
                               preset->aspect_ratio.label);
    }
}

void CropGuide::onAspectRatioPresetPickColor(size_t index, ColorPreview* preview)
{
    auto& preset = m_aspect_ratio_presets.at(index);

    Gtk::ColorChooserDialog dialog;
    dialog.set_use_alpha(false);
    dialog.set_rgba(preset->color);

    int result = dialog.run();
    if (result != Gtk::RESPONSE_OK) return;

    Gdk::RGBA color = dialog.get_rgba();
    preset->color = color;
    m_dirty_aspect_ratios = true;

    preview->setRgb(color.get_red(), color.get_green(), color.get_blue());

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuideAspectRatioPresetChanged,
                               preset->aspect_ratio.label);
    }
}

void CropGuide::onAspectRatioPresetRemoved(size_t index)
{
    auto& preset = m_aspect_ratio_presets.at(index);
    preset->active = false;
    preset->visible = false;
    m_dirty_aspect_ratios = true;

    auto size = m_aspect_ratio_store->get_n_items();
    for (guint i = 0; i < size; i++) {
        if (m_aspect_ratio_store->get_object(i) == preset) {
            m_aspect_ratio_store->remove(i);
            break;
        }
    }

    refreshAvailableAspectRatios();

    if (listener && getEnabled()) {
        listener->panelChanged(EvCropGuideAspectRatioPresetChanged,
                               preset->aspect_ratio.label);
    }
}

void CropGuide::refreshAvailableAspectRatios()
{
    ConnectionBlocker block(m_available_aspect_ratios_conn);
    m_available_aspect_ratios_combo->remove_all();
    for (const auto& model : m_aspect_ratio_presets) {
        if (!model->active) {
            m_available_aspect_ratios_combo->append(model->aspect_ratio.label);
        }
    }
}

int CropGuide::compareAspectRatioModels(
    const Glib::RefPtr<const AspectRatioModel>& lhs,
    const Glib::RefPtr<const AspectRatioModel>& rhs) const
{
    if (lhs->index < rhs->index) {
        return -1;
    } else if (lhs->index > rhs->index) {
        return 1;
    } else {
        return 0;
    }
}
