//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPrompt.cpp
//
//  Offers a Bluetooth audio device as the audio output when it connects.
//

#include "BluetoothAudioPrompt.h"

#include "Settings.h"
#include "UIModeController.h"
#include "Window.h"
#include "audio/AudioOutputManager.h"
#include "bluetooth/BluetoothManager.h"
#include "guis/GuiMsgBox.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

#include <SDL2/SDL_timer.h>

BluetoothAudioPrompt& BluetoothAudioPrompt::getInstance()
{
    static BluetoothAudioPrompt instance;
    return instance;
}

void BluetoothAudioPrompt::update()
{
    if (!Settings::getInstance()->getBool("BluetoothPromptAudio"))
        return;

    // The prompt belongs to the audio output selection, which only exists in strictly full UI mode.
    UIModeController* uiMode {UIModeController::getInstance()};
    if (!uiMode->isUIModeFull() || uiMode->isUIModeKid())
        return;

    // Nothing is offered while a game is being launched, and not while one is running in the
    // background either. The observation is skipped rather than evaluated and dropped, so a sink
    // that is gone by the time the game ends is simply forgotten.
    Window* window {Window::getInstance()};
    if (window->isLaunchScreenDisplayed() || window->getGameLaunchedState())
        return;

    // One prompt at a time. The next observation after the answer starts the grace period of
    // whatever else is waiting.
    if (mPromptOpen)
        return;

    const std::shared_ptr<const AudioSnapshot> audio {
        AudioOutputManager::getInstance().getSnapshot()};
    if (audio == nullptr)
        return;

    const std::shared_ptr<const BluetoothSnapshot> bluetooth {
        BluetoothManager::getInstance().getSnapshot()};
    if (bluetooth == nullptr)
        return;

    const BluetoothAudioPromptTracker::Offer offer {
        mTracker.update(*audio, *bluetooth, SDL_GetTicks())};

    if (offer.outputId.empty())
        return;

    showPrompt(offer);
}

void BluetoothAudioPrompt::showPrompt(const BluetoothAudioPromptTracker::Offer& offer)
{
    const std::string name {offer.deviceName.empty() ? _("A BLUETOOTH DEVICE") :
                                                       Utils::String::toUpper(offer.deviceName)};
    const std::string outputId {offer.outputId};

    mPromptOpen = true;

    Window::getInstance()->pushGui(new GuiMsgBox(
        Utils::String::format(_("%s IS CONNECTED. USE IT AS THE AUDIO OUTPUT?"), name.c_str()),
        _("YES"),
        [this, outputId] {
            mPromptOpen = false;
            AudioOutputManager::getInstance().setDefault(outputId);
        },
        _("NO"), [this] { mPromptOpen = false; }, "", nullptr, "", nullptr,
        [this] { mPromptOpen = false; }));
}
