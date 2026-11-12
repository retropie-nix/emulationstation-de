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
#include <vector>

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

    // The parts of the snapshot that have to reach the screen without waiting for the throttle: the
    // adapter state, the scan state and the operation in flight of every device.
    static std::string immediateState(const BluetoothSnapshot& snapshot);

    // How long a scan runs before it is stopped, same order of magnitude as a phone.
    static constexpr int sScanTime {30000};

    // Rebuilding at most twice a second keeps the signal strength figures usable without moving the
    // list under the cursor all the time.
    static constexpr int sRebuildInterval {500};

    std::uint64_t mSequence {0};
    std::string mLastError;
    std::string mImmediateState;
    int mScanElapsed {0};
    int mRebuildElapsed {0};
    // The device behind each row, empty for the adapter and scan rows. Used to keep the cursor on
    // the same device when the rows above it change.
    std::vector<std::string> mRowDevices;
};

#endif // ES_APP_GUIS_GUI_BLUETOOTH_SETTINGS_H
