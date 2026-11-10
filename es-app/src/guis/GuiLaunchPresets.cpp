//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiLaunchPresets.cpp
//
//  User interface to select a launch preset (an es_systems.xml <command> entry)
//  when launching a game.
//

#include "guis/GuiLaunchPresets.h"

#include "FileData.h"
#include "SystemData.h"
#include "components/TextComponent.h"
#include "utils/LocalizationUtil.h"
#include "views/ViewController.h"

GuiLaunchPresets::GuiLaunchPresets(FileData* game)
    : mMenu {_("LAUNCH PRESETS")}
{
    addChild(&mMenu);
    mMenu.addButton(_("BACK"), _("back"), [this] { delete this; });

    SystemEnvironmentData* envData {game->getSourceFileData()->getSystem()->getSystemEnvData()};
    const std::string defaultLabel {game->getSourceFileData()->getDefaultLaunchPresetLabel()};
    const std::string firstLabel {envData->mLaunchCommands.front().second};

    for (auto& entry : envData->mLaunchCommands) {
        ComponentListRow row;

        std::string label {entry.second.empty() ? _("DEFAULT") : entry.second};

        if (!firstLabel.empty() && entry.second == firstLabel)
            label.append(" [").append(_("DEFAULT")).append("]");
        else if (!defaultLabel.empty() && entry.second == defaultLabel)
            label.append(" [").append(_("PREFERRED")).append("]");

        const bool isPreferred {(entry.second == defaultLabel && !defaultLabel.empty()) ||
                                (defaultLabel.empty() && entry.second == firstLabel)};

        std::shared_ptr<TextComponent> labelText {std::make_shared<TextComponent>(
            label,
            Font::get(FONT_SIZE_MEDIUM, isPreferred ? FONT_PATH_BOLD : FONT_PATH_REGULAR),
            mMenuColorPrimary, ALIGN_LEFT)};
        labelText->setSelectable(true);
        row.addElement(labelText, true);

        const std::string presetLabel {entry.second};
        row.makeAcceptInputHandler([this, game, presetLabel] {
            delete this;
            ViewController::getInstance()->launchGameWithPreset(game, presetLabel);
        });

        mMenu.addRow(row);
    }

    setSize(mMenu.getSize());
    setPosition((Renderer::getScreenWidth() - mSize.x) / 2.0f, Renderer::getScreenHeight() * 0.30f);
}
