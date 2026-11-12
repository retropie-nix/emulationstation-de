//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPrompt.cpp
//
//  Offers a Bluetooth audio device as the audio output when it connects.
//

#include "BluetoothAudioPrompt.h"

#include "Settings.h"
#include "Window.h"
#include "audio/AudioOutputManager.h"
#include "bluetooth/BluetoothManager.h"
#include "guis/GuiMsgBox.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

namespace
{
    bool isSameAddress(const std::string& left, const std::string& right)
    {
        return left.size() == right.size() &&
               Utils::String::toLower(left) == Utils::String::toLower(right);
    }

    bool isAudioDevice(const BluetoothDeviceKind kind)
    {
        return kind == BluetoothDeviceKind::headset || kind == BluetoothDeviceKind::speaker;
    }

} // namespace

BluetoothAudioPrompt& BluetoothAudioPrompt::getInstance()
{
    static BluetoothAudioPrompt instance;
    return instance;
}

void BluetoothAudioPrompt::update()
{
    if (!Settings::getInstance()->getBool("BluetoothPromptAudio"))
        return;

    // Nothing is offered while a game is being launched, and not while one is running in the
    // background either.
    Window* window {Window::getInstance()};
    if (window->isLaunchScreenDisplayed() || window->getGameLaunchedState())
        return;

    AudioOutputManager& audioManager {AudioOutputManager::getInstance()};
    const std::shared_ptr<const AudioSnapshot> audio {audioManager.getSnapshot()};
    if (audio == nullptr)
        return;

    BluetoothManager& bluetoothManager {BluetoothManager::getInstance()};
    const std::shared_ptr<const BluetoothSnapshot> bluetooth {bluetoothManager.getSnapshot()};
    if (bluetooth == nullptr)
        return;

    // Both managers publish a snapshot for every change, so nothing has to be looked at while
    // neither of them has changed.
    if (audio->sequence == mAudioSequence && bluetooth->sequence == mBluetoothSequence)
        return;

    mAudioSequence = audio->sequence;
    mBluetoothSequence = bluetooth->sequence;

    // An output that is gone was disconnected, so it can be offered again when it comes back.
    std::set<std::string> available;
    for (const AudioOutput& output : audio->outputs)
        available.insert(output.id);

    for (auto it = mOfferedOutputs.begin(); it != mOfferedOutputs.end();) {
        if (available.find(*it) == available.end())
            it = mOfferedOutputs.erase(it);
        else
            ++it;
    }

    if (mPromptOpen)
        return;

    for (const AudioOutput& output : audio->outputs) {
        if (output.bluetoothAddress.empty())
            continue;

        // An output that is already the default was accepted before, or restored by the sound
        // server, and must not be offered again.
        if (output.id == audio->defaultId)
            continue;

        if (mOfferedOutputs.find(output.id) != mOfferedOutputs.end())
            continue;

        const BluetoothDevice* device {nullptr};
        for (const BluetoothDevice& candidate : bluetooth->devices) {
            if (isAudioDevice(candidate.kind) &&
                isSameAddress(candidate.address, output.bluetoothAddress)) {
                device = &candidate;
                break;
            }
        }

        if (device == nullptr)
            continue;

        // Remembered before the prompt is shown, so that a snapshot that arrives while it is open
        // cannot offer the same output twice.
        mOfferedOutputs.insert(output.id);
        showPrompt(output.id, device->name.empty() ? output.description : device->name);
        return;
    }
}

void BluetoothAudioPrompt::showPrompt(const std::string& outputId, const std::string& deviceName)
{
    const std::string name {deviceName.empty() ? _("A BLUETOOTH DEVICE") :
                                                Utils::String::toUpper(deviceName)};

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
