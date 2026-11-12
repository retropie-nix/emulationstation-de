//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPromptTracker.cpp
//
//  Decides when a Bluetooth audio sink may be offered as the audio output.
//

#include "BluetoothAudioPromptTracker.h"

#include <cctype>
#include <set>

namespace
{
    bool isAudioDevice(const BluetoothDeviceKind kind)
    {
        return kind == BluetoothDeviceKind::headset || kind == BluetoothDeviceKind::speaker;
    }

} // namespace

bool BluetoothAudioPromptTracker::isSameAddress(const std::string& left, const std::string& right)
{
    if (left.size() != right.size())
        return false;

    for (std::size_t i {0}; i < left.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(left.at(i))) !=
            std::toupper(static_cast<unsigned char>(right.at(i))))
            return false;
    }

    return true;
}

const BluetoothDevice* BluetoothAudioPromptTracker::findAudioDevice(
    const BluetoothSnapshot& bluetooth,
    const std::string& address)
{
    for (const BluetoothDevice& device : bluetooth.devices) {
        if (isAudioDevice(device.kind) && isSameAddress(device.address, address))
            return &device;
    }

    return nullptr;
}

BluetoothAudioMode BluetoothAudioPromptTracker::modeFromString(const std::string& value)
{
    if (value == "auto")
        return BluetoothAudioMode::autoSwitch;
    if (value == "off")
        return BluetoothAudioMode::off;

    return BluetoothAudioMode::ask;
}

BluetoothAudioPromptTracker::Offer BluetoothAudioPromptTracker::update(
    const AudioSnapshot& audio,
    const BluetoothSnapshot& bluetooth,
    const BluetoothAudioMode mode,
    const std::uint32_t now)
{
    // A sink that is gone was disconnected, so both its grace period and the fact that it was
    // offered are forgotten and a later reconnection can be offered again.
    std::set<std::string> present;
    for (const AudioOutput& output : audio.outputs)
        present.insert(output.id);

    for (auto it = mSinks.begin(); it != mSinks.end();) {
        if (present.find(it->first) == present.end())
            it = mSinks.erase(it);
        else
            ++it;
    }

    Offer offer;

    for (const AudioOutput& output : audio.outputs) {
        if (output.bluetoothAddress.empty())
            continue;

        const BluetoothDevice* device {findAudioDevice(bluetooth, output.bluetoothAddress)};

        // The state is re-checked on every observation, so a sink that became the default, whose
        // device disconnected or that is the default already never reaches the offer below.
        const bool isCandidate {device != nullptr && device->connected &&
                                output.id != audio.defaultId};

        if (!isCandidate) {
            const auto sink {mSinks.find(output.id)};
            if (sink != mSinks.end())
                sink->second.waiting = false;

            continue;
        }

        Sink& sink {mSinks[output.id]};

        if (!sink.waiting) {
            // The grace period starts now; there is nothing to offer in this observation.
            sink.waiting = true;
            sink.firstSeen = now;
            continue;
        }

        if (sink.offered)
            continue;

        if (static_cast<std::uint32_t>(now - sink.firstSeen) >= sGracePeriod) {
            // With the off mode the device is never offered, but neither is it remembered as
            // offered, so turning the setting back on can still offer it for this connection.
            if (mode == BluetoothAudioMode::off)
                return offer;

            sink.offered = true;

            offer.outputId = output.id;
            offer.deviceName = device->name.empty() ? output.description : device->name;
            offer.action = mode == BluetoothAudioMode::autoSwitch ? BluetoothAudioOffer::switchOutput :
                                                                   BluetoothAudioOffer::prompt;

            return offer;
        }
    }

    return offer;
}
