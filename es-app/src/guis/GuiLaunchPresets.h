//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiLaunchPresets.h
//
//  User interface to select a launch preset (an es_systems.xml <command> entry)
//  when launching a game.
//

#ifndef ES_APP_GUIS_GUI_LAUNCH_PRESETS_H
#define ES_APP_GUIS_GUI_LAUNCH_PRESETS_H

#include "GuiComponent.h"
#include "guis/GuiSettings.h"

class FileData;

class GuiLaunchPresets : public GuiComponent
{
public:
    explicit GuiLaunchPresets(FileData* game);

private:
    MenuComponent mMenu;
};

#endif // ES_APP_GUIS_GUI_LAUNCH_PRESETS_H
