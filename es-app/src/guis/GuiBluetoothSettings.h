//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiBluetoothSettings.h
//
//  Bluetooth device management screen.
//
//  Every row is rebuilt from the manager's snapshot when it publishes a new one, so this screen is
//  a view of the Bluetooth state instead of a form that is filled in and saved. All actions are
//  sent directly to the manager by their rows.
//

#ifndef ES_APP_GUIS_GUI_BLUETOOTH_SETTINGS_H
#define ES_APP_GUIS_GUI_BLUETOOTH_SETTINGS_H

#include "bluetooth/BluetoothManager.h"
#include "guis/GuiSettings.h"

#include <cstdint>
#include <string>

class GuiBluetoothSettings : public GuiSettings
{
public:
    GuiBluetoothSettings();
    ~GuiBluetoothSettings() override;

    void update(int deltaTime) override;

private:
    // Rebuilds every row from the current snapshot, keeping the cursor where it was.
    void rebuild();

    // The PAIR, CONNECT, DISCONNECT and FORGET screen of one device.
    void openDeviceActions(const std::string& devicePath);

    void toggleScan();

    static std::string deviceLabel(const BluetoothDevice& device);
    static std::string stateLabel(const BluetoothDevice& device);
    static std::string kindLabel(BluetoothDeviceKind kind);

    // How long a scan runs before it is stopped, same order of magnitude as a phone.
    static constexpr int sScanTime {30000};

    std::uint64_t mSequence {0};
    std::string mLastError;
    int mScanElapsed {0};
};

#endif // ES_APP_GUIS_GUI_BLUETOOTH_SETTINGS_H
