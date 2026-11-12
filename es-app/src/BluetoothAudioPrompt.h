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

class GuiComponent;

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

    // The message box that is waiting for an answer, null when none is. Holding the component
    // instead of a flag means that a box which disappears without a callback, which happens when
    // the GUI stack is torn down, no longer suppresses the next prompt.
    GuiComponent* mPromptGui {nullptr};
};

#endif // ES_APP_BLUETOOTH_AUDIO_PROMPT_H
