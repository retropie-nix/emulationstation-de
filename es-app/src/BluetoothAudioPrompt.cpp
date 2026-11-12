//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPrompt.cpp
//
//  Offers a Bluetooth audio device as the audio output when it connects.
//

#include "BluetoothAudioPrompt.h"

#include "AudioManager.h"
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
    const BluetoothAudioMode mode {BluetoothAudioPromptTracker::modeFromString(
        Settings::getInstance()->getString("BluetoothAudioMode"))};

    if (mode == BluetoothAudioMode::off)
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
    // whatever else is waiting. A box that is no longer on the screen never got its callback, so
    // it must not keep the next prompt away.
    if (mPromptGui != nullptr) {
        if (window->peekGui() == mPromptGui)
            return;

        mPromptGui = nullptr;
    }

    const std::shared_ptr<const AudioSnapshot> audio {
        AudioOutputManager::getInstance().getSnapshot()};
    if (audio == nullptr)
        return;

    const std::shared_ptr<const BluetoothSnapshot> bluetooth {
        BluetoothManager::getInstance().getSnapshot()};
    if (bluetooth == nullptr)
        return;

    const BluetoothAudioPromptTracker::Offer offer {
        mTracker.update(*audio, *bluetooth, mode, SDL_GetTicks())};

    if (offer.action == BluetoothAudioOffer::switchOutput) {
        // The auto mode switches without asking, and the device is reopened so that menu audio
        // follows the new default.
        AudioOutputManager::getInstance().setDefault(offer.outputId);
        AudioManager::getInstance().requestAudioReopenForOutput(offer.outputId);
        return;
    }

    if (offer.action != BluetoothAudioOffer::prompt)
        return;

    showPrompt(offer);
}

void BluetoothAudioPrompt::showPrompt(const BluetoothAudioPromptTracker::Offer& offer)
{
    const std::string name {offer.deviceName.empty() ? _("A BLUETOOTH DEVICE") :
                                                       Utils::String::toUpper(offer.deviceName)};
    const std::string outputId {offer.outputId};

    auto* box {new GuiMsgBox(
        Utils::String::format(_("%s IS CONNECTED. USE IT AS THE AUDIO OUTPUT?"), name.c_str()),
        _("YES"),
        [this, outputId] {
            mPromptGui = nullptr;
            AudioOutputManager::getInstance().setDefault(outputId);
            // Menu audio follows the accepted device once the sound server confirms the default.
            AudioManager::getInstance().requestAudioReopenForOutput(outputId);
        },
        _("NO"), [this] { mPromptGui = nullptr; }, "", nullptr, "", nullptr,
        [this] { mPromptGui = nullptr; })};

    mPromptGui = box;
    Window::getInstance()->pushGui(box);
}
