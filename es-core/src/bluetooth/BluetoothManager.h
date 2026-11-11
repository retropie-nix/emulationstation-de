//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothManager.h
//
//  Bluetooth device management interface.
//
//  The implementation is selected at build time and hides the platform details behind an
//  asynchronous action API: actions return immediately and the resulting state is published as an
//  immutable snapshot that the UI reads once per frame.
//

#ifndef ES_CORE_BLUETOOTH_MANAGER_H
#define ES_CORE_BLUETOOTH_MANAGER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The operation that is currently in flight for a device. The UI uses this to disable the
// conflicting actions and to show that something is happening.
enum class BluetoothOp {
    none,
    pairing,
    connecting,
    disconnecting,
    removing
};

// What a device is, from the point of view of the UI. Derived from the service UUIDs, the icon
// and the appearance, so that it also works before a device was ever connected.
enum class BluetoothDeviceKind {
    other,
    controller,
    keyboard,
    mouse,
    headset,
    speaker
};

struct BluetoothDevice {
    // The BlueZ object path, which is the identity of the device.
    std::string path;
    std::string address;
    std::string name;
    // The BlueZ Icon property, for example "input-gaming" or "audio-card".
    std::string icon;
    // The BlueZ service UUIDs, used to classify the device.
    std::vector<std::string> uuids;
    // The Bluetooth appearance value, 0 when the device does not report one.
    std::uint32_t appearance {0};
    // The Bluetooth device class, 0 when the device does not report one. Devices that only speak
    // Bluetooth Low Energy have no class.
    std::uint32_t deviceClass {0};
    bool paired {false};
    bool connected {false};
    bool trusted {false};
    // Battery percentage, -1 when unknown.
    int battery {-1};
    // Signal strength, 0 when unknown.
    int rssi {0};
    BluetoothDeviceKind kind {BluetoothDeviceKind::other};
    BluetoothOp pending {BluetoothOp::none};
};

struct BluetoothSnapshot {
    // True when a usable adapter was found. Everything else in the snapshot is meaningless when
    // this is false.
    bool available {false};
    bool powered {false};
    std::string adapterPath;
    std::string adapterName;
    std::string adapterAddress;
    bool discovering {false};
    std::vector<BluetoothDevice> devices;
    // The last failure, in a form that can be shown to the user. Empty when there is none.
    std::string lastError;
    // Increases every time a snapshot is published.
    std::uint64_t sequence {0};
};

class BluetoothManager
{
public:
    virtual ~BluetoothManager() = default;

    static BluetoothManager& getInstance();

    BluetoothManager(const BluetoothManager&) = delete;
    BluetoothManager& operator=(const BluetoothManager&) = delete;

    // False in builds without a backend and, with a backend, while no adapter is present.
    virtual bool isAvailable() const = 0;

    // The current state. The pointer stays valid when a newer snapshot is published, so the UI
    // can hold on to it while it draws a frame.
    virtual std::shared_ptr<const BluetoothSnapshot> getSnapshot() const = 0;

    // The sequence number of the current snapshot, a cheap way to detect that it changed.
    virtual std::uint64_t getSequence() const = 0;

    // The actions below return immediately. Their progress and outcome are reported through the
    // snapshot: BluetoothOp::pending on the device that is being worked on, and
    // BluetoothSnapshot::lastError for failures.
    virtual void setPowered(const bool powered) = 0;
    virtual void startDiscovery() = 0;
    virtual void stopDiscovery() = 0;
    virtual void pair(const std::string& devicePath) = 0;
    virtual void connect(const std::string& devicePath) = 0;
    virtual void disconnect(const std::string& devicePath) = 0;
    virtual void forget(const std::string& devicePath) = 0;

    // Pairing mode, which the Bluetooth screen turns on while it is open. With a backend it
    // registers the pairing agent and keeps the adapter pairable; without one it does nothing.
    virtual void setPairingEnabled(const bool enabled) = 0;

protected:
    BluetoothManager() = default;
};

#endif // ES_CORE_BLUETOOTH_MANAGER_H
