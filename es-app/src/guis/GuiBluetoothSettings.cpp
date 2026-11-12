//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  GuiBluetoothSettings.cpp
//
//  Bluetooth device management screen.
//

#include "guis/GuiBluetoothSettings.h"

#include "components/ComponentList.h"
#include "components/SwitchComponent.h"
#include "components/TextComponent.h"
#include "guis/GuiMsgBox.h"
#include "resources/Font.h"
#include "utils/StringUtil.h"

#include <algorithm>
#include <functional>

GuiBluetoothSettings::GuiBluetoothSettings()
    : GuiSettings {_("BLUETOOTH")}
{
    // While this screen is open the frontend is the pairing agent and the adapter is pairable.
    BluetoothManager::getInstance().setPairingEnabled(true);
    rebuild();
}

GuiBluetoothSettings::~GuiBluetoothSettings()
{
    BluetoothManager& manager {BluetoothManager::getInstance()};
    const std::shared_ptr<const BluetoothSnapshot> snapshot {manager.getSnapshot()};

    // A scan must not outlive the screen, and neither must the pairing mode.
    if (snapshot != nullptr && snapshot->discovering)
        manager.stopDiscovery();

    manager.setPairingEnabled(false);
}

void GuiBluetoothSettings::update(int deltaTime)
{
    GuiSettings::update(deltaTime);

    BluetoothManager& manager {BluetoothManager::getInstance()};
    const std::shared_ptr<const BluetoothSnapshot> snapshot {manager.getSnapshot()};
    if (snapshot == nullptr)
        return;

    // Failures are shown once, when they appear.
    if (snapshot->lastError != mLastError) {
        mLastError = snapshot->lastError;
        if (!mLastError.empty())
            mWindow->pushGui(new GuiMsgBox(Utils::String::toUpper(mLastError)));
    }

    // A scan stops by itself after a while, so the adapter does not keep inquiring.
    if (snapshot->discovering) {
        mScanElapsed += deltaTime;
        if (mScanElapsed >= sScanTime) {
            mScanElapsed = 0;
            manager.stopDiscovery();
        }
    }
    else {
        mScanElapsed = 0;
    }

    // The manager publishes a new snapshot for every change, the signal strength and the battery of
    // every device included, so rebuilding on each of them would move the list under the cursor.
    // The state the user acts on is applied immediately and everything else waits for the next
    // rebuild at most half a second away.
    if (snapshot->sequence == mSequence)
        return;

    mRebuildElapsed += deltaTime;

    const std::string immediate {immediateState(*snapshot)};
    if (immediate != mImmediateState || mRebuildElapsed >= sRebuildInterval) {
        mSequence = snapshot->sequence;
        mRebuildElapsed = 0;
        rebuild();
    }
}

void GuiBluetoothSettings::rebuild()
{
    const std::shared_ptr<const BluetoothSnapshot> snapshot {
        BluetoothManager::getInstance().getSnapshot()};

    const std::shared_ptr<ComponentList> list {getMenu().getList()};
    const int previousCursor {list->getCursorId()};
    const std::string cursorDevice {(previousCursor >= 0 &&
                                     previousCursor < static_cast<int>(mRowDevices.size())) ?
                                        mRowDevices.at(previousCursor) :
                                        std::string {}};

    list->clear();
    mRowDevices.clear();

    // Every row is added through this, so that the device behind it is known and the cursor can be
    // put back on the same device after the rows above it changed.
    auto addRow = [this](const ComponentListRow& row, const std::string& devicePath) {
        mRowDevices.push_back(devicePath);
        getMenu().addRow(row);
    };

    mImmediateState = snapshot == nullptr ? std::string {} : immediateState(*snapshot);

    if (snapshot == nullptr || !snapshot->available) {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>(_("BLUETOOTH IS NOT AVAILABLE"),
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorSecondary, ALIGN_LEFT),
                       true);
        addRow(row, std::string {});
        return;
    }

    // The adapter, as a power switch and its address.
    auto powerSwitch {std::make_shared<SwitchComponent>(snapshot->powered)};
    // The callback is stored in the switch itself, so it must not own it.
    SwitchComponent* switchPointer {powerSwitch.get()};
    powerSwitch->setCallback([switchPointer] {
        BluetoothManager::getInstance().setPowered(switchPointer->getState());
    });

    ComponentListRow powerRow;
    powerRow.addElement(std::make_shared<TextComponent>(_("POWER"), Font::get(FONT_SIZE_MEDIUM),
                                                        mMenuColorPrimary, ALIGN_LEFT),
                        true);
    powerRow.addElement(powerSwitch, false);
    addRow(powerRow, std::string {});

    ComponentListRow adapterRow;
    adapterRow.addElement(std::make_shared<TextComponent>(snapshot->adapterName + " (" +
                                                              snapshot->adapterAddress + ")",
                                                          Font::get(FONT_SIZE_SMALL),
                                                          mMenuColorSecondary, ALIGN_LEFT),
                          true);
    addRow(adapterRow, std::string {});

    if (snapshot->powered) {
        ComponentListRow scanRow;
        scanRow.addElement(std::make_shared<TextComponent>(snapshot->discovering ?
                                                               _("STOP SCANNING") :
                                                               _("SCAN FOR DEVICES"),
                                                           Font::get(FONT_SIZE_MEDIUM),
                                                           mMenuColorPrimary, ALIGN_LEFT),
                           true);

        if (snapshot->discovering) {
            // The number of devices found so far is the only progress there is during a scan.
            scanRow.addElement(std::make_shared<TextComponent>(
                                   std::to_string(snapshot->devices.size()),
                                   Font::get(FONT_SIZE_SMALL), mMenuColorSecondary, ALIGN_RIGHT),
                               false);
        }

        scanRow.makeAcceptInputHandler([this] { toggleScan(); });
        addRow(scanRow, std::string {});
    }

    for (const BluetoothDevice& device : snapshot->devices) {
        ComponentListRow row;

        auto name {std::make_shared<TextComponent>(deviceLabel(device), Font::get(FONT_SIZE_MEDIUM),
                                                   mMenuColorPrimary, ALIGN_LEFT)};
        name->setHorizontalScrolling(true);
        row.addElement(name, true);
        row.addElement(
            std::make_shared<TextComponent>(stateLabel(device), Font::get(FONT_SIZE_SMALL),
                                            mMenuColorSecondary, ALIGN_RIGHT),
            false);

        const std::string devicePath {device.path};
        row.makeAcceptInputHandler([this, devicePath] { openDeviceActions(devicePath); });
        addRow(row, devicePath);
    }

    // Stay on the same device while the list changes underneath. The row index is only a fallback,
    // because the adapter and scan rows move when the adapter is powered on or off.
    const int rows {static_cast<int>(mRowDevices.size())};
    if (rows == 0)
        return;

    int target {std::clamp(previousCursor, 0, rows - 1)};

    if (!cursorDevice.empty()) {
        const auto found {std::find(mRowDevices.cbegin(), mRowDevices.cend(), cursorDevice)};

        if (found != mRowDevices.cend())
            target = static_cast<int>(found - mRowDevices.cbegin());
    }

    if (target != list->getCursorId())
        list->moveCursor(target - list->getCursorId());
}

void GuiBluetoothSettings::openDeviceActions(const std::string& devicePath)
{
    const std::shared_ptr<const BluetoothSnapshot> snapshot {
        BluetoothManager::getInstance().getSnapshot()};
    if (snapshot == nullptr)
        return;

    const BluetoothDevice* device {nullptr};
    for (const BluetoothDevice& candidate : snapshot->devices) {
        if (candidate.path == devicePath) {
            device = &candidate;
            break;
        }
    }

    if (device == nullptr)
        return;

    auto settings = new GuiSettings(_("BLUETOOTH DEVICE"));

    ComponentListRow header;
    header.addElement(std::make_shared<TextComponent>(deviceLabel(*device),
                                                      Font::get(FONT_SIZE_MEDIUM),
                                                      mMenuColorPrimary, ALIGN_LEFT),
                      true);
    header.addElement(std::make_shared<TextComponent>(device->address, Font::get(FONT_SIZE_SMALL),
                                                      mMenuColorSecondary, ALIGN_RIGHT),
                      false);
    settings->addRow(header);

    const bool busy {device->pending != BluetoothOp::none};

    // Each action is sent straight to the manager, and the screen closes so that the progress of
    // the operation is visible in the rebuilt list.
    auto addAction = [settings](const std::string& label, const bool enabled,
                                const std::function<void()>& action) {
        ComponentListRow row;
        auto text {std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                   mMenuColorPrimary, ALIGN_LEFT)};

        if (!enabled) {
            // Shown but not selectable, so the reason a device cannot be connected is visible.
            text->setOpacity(0.5f);
        }
        else {
            row.makeAcceptInputHandler([settings, action] {
                action();
                delete settings;
            });
        }

        row.addElement(text, true);
        settings->addRow(row);
    };

    addAction(_("PAIR"), !busy && !device->paired,
              [devicePath] { BluetoothManager::getInstance().pair(devicePath); });
    // Connecting an unpaired device would make BlueZ pair it implicitly, which bypasses the trust
    // the pair action sets up and is refused by the agent.
    addAction(_("CONNECT"), !busy && device->paired && !device->connected,
              [devicePath] { BluetoothManager::getInstance().connect(devicePath); });
    addAction(_("DISCONNECT"), !busy && device->connected,
              [devicePath] { BluetoothManager::getInstance().disconnect(devicePath); });
    addAction(_("FORGET"), !busy && (device->paired || device->connected),
              [devicePath] { BluetoothManager::getInstance().forget(devicePath); });

    mWindow->pushGui(settings);
}

void GuiBluetoothSettings::toggleScan()
{
    BluetoothManager& manager {BluetoothManager::getInstance()};
    const std::shared_ptr<const BluetoothSnapshot> snapshot {manager.getSnapshot()};
    if (snapshot == nullptr)
        return;

    mScanElapsed = 0;

    if (snapshot->discovering)
        manager.stopDiscovery();
    else
        manager.startDiscovery();
}

std::string GuiBluetoothSettings::immediateState(const BluetoothSnapshot& snapshot)
{
    std::string state;
    state += snapshot.available ? '1' : '0';
    state += snapshot.powered ? '1' : '0';
    state += snapshot.discovering ? '1' : '0';

    // A device that changes what it is doing has to show up at once, and so does the moment its
    // operation ends. The signal strength and the battery can wait for the next rebuild.
    for (const BluetoothDevice& device : snapshot.devices) {
        if (device.pending == BluetoothOp::none)
            continue;

        state += device.path;
        state += static_cast<char>('0' + static_cast<int>(device.pending));
    }

    return state;
}

std::string GuiBluetoothSettings::deviceLabel(const BluetoothDevice& device)
{
    const std::string name {
        Utils::String::toUpper(device.name.empty() ? device.address : device.name)};
    const std::string kind {kindLabel(device.kind)};

    if (kind.empty())
        return name;

    return name + " (" + kind + ")";
}

std::string GuiBluetoothSettings::stateLabel(const BluetoothDevice& device)
{
    std::string state;

    switch (device.pending) {
        case BluetoothOp::pairing:
            state = _("PAIRING");
            break;
        case BluetoothOp::connecting:
            state = _("CONNECTING");
            break;
        case BluetoothOp::disconnecting:
            state = _("DISCONNECTING");
            break;
        case BluetoothOp::removing:
            state = _("REMOVING");
            break;
        case BluetoothOp::none:
            if (device.connected)
                state = _("CONNECTED");
            else if (device.paired)
                state = _("PAIRED");
            else
                state = _("NOT PAIRED");
            break;
    }

    std::string text {device.address + "  " + state};

    if (device.battery >= 0)
        text += "  " + std::to_string(device.battery) + "%";

    return text;
}

std::string GuiBluetoothSettings::kindLabel(const BluetoothDeviceKind kind)
{
    switch (kind) {
        case BluetoothDeviceKind::controller:
            return _("CONTROLLER");
        case BluetoothDeviceKind::keyboard:
            return _("KEYBOARD");
        case BluetoothDeviceKind::mouse:
            return _("MOUSE");
        case BluetoothDeviceKind::headset:
            return _("HEADSET");
        case BluetoothDeviceKind::speaker:
            return _("SPEAKER");
        case BluetoothDeviceKind::other:
            break;
    }

    return std::string {};
}
