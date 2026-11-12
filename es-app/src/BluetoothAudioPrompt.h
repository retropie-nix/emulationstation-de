//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothAudioPrompt.h
//
//  Offers a Bluetooth audio device as the audio output when it connects.
//
//  The prompt is driven from the application loop rather than from a screen, so that it appears no
//  matter what is on the display, and it never fires while a game is being launched or is running.
//

#ifndef ES_APP_BLUETOOTH_AUDIO_PROMPT_H
#define ES_APP_BLUETOOTH_AUDIO_PROMPT_H

#include <cstdint>
#include <set>
#include <string>

class BluetoothAudioPrompt
{
public:
    static BluetoothAudioPrompt& getInstance();

    // Called once per frame.
    void update();

private:
    BluetoothAudioPrompt() = default;

    void showPrompt(const std::string& outputId, const std::string& deviceName);

    // The outputs that were already offered for their current connection. An output is forgotten
    // again when it disappears from the snapshot, so that a later reconnection can be offered.
    std::set<std::string> mOfferedOutputs;

    std::uint64_t mAudioSequence {0};
    std::uint64_t mBluetoothSequence {0};
    bool mPromptOpen {false};
};

#endif // ES_APP_BLUETOOTH_AUDIO_PROMPT_H
