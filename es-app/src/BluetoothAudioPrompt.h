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

#include "BluetoothAudioPromptTracker.h"

class BluetoothAudioPrompt
{
public:
    static BluetoothAudioPrompt& getInstance();

    // Called once per frame.
    void update();

private:
    BluetoothAudioPrompt() = default;

    void showPrompt(const BluetoothAudioPromptTracker::Offer& offer);

    BluetoothAudioPromptTracker mTracker;
    bool mPromptOpen {false};
};

#endif // ES_APP_BLUETOOTH_AUDIO_PROMPT_H
