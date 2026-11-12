//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothState.h
//
//  Platform independent reduction of the BlueZ D-Bus objects into a BluetoothSnapshot.
//
//  The BlueZ backend feeds the objects it reads from D-Bus into BluetoothState and publishes the
//  snapshot it produces. Keeping the reduction here instead of in the sd-bus code makes the
//  classification, the ordering and the error mapping testable without a bus.
//

#ifndef ES_CORE_BLUETOOTH_STATE_H
#define ES_CORE_BLUETOOTH_STATE_H

#include "bluetooth/BluetoothManager.h"

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

// A D-Bus property reduced to the value types that the BlueZ interfaces use. All signed integers
// are reduced to std::int64_t and all unsigned ones to std::uint64_t.
using BluetoothPropertyValue =
    std::variant<bool, std::int64_t, std::uint64_t, std::string, std::vector<std::string>>;

// The properties of one interface, by property name.
using BluetoothProperties = std::map<std::string, BluetoothPropertyValue>;

// The interfaces of one object, by interface name.
using BluetoothInterfaces = std::map<std::string, BluetoothProperties>;

// The managed objects of org.freedesktop.DBus.ObjectManager, by object path.
using BluetoothObjectMap = std::map<std::string, BluetoothInterfaces>;

class BluetoothState
{
public:
    // The only interfaces that are kept.
    static constexpr const char* adapterInterface {"org.bluez.Adapter1"};
    static constexpr const char* deviceInterface {"org.bluez.Device1"};
    static constexpr const char* batteryInterface {"org.bluez.Battery1"};

    // True for the three interfaces above, everything else is dropped while parsing.
    static bool isKeptInterface(const std::string& interfaceName);

    // Replaces the state with the result of GetManagedObjects.
    void setManagedObjects(const BluetoothObjectMap& objects);

    // Applies one signal of org.freedesktop.DBus.ObjectManager.
    void applyInterfacesAdded(const std::string& path, const BluetoothInterfaces& interfaces);
    void applyInterfacesRemoved(const std::string& path,
                                const std::vector<std::string>& interfaces);

    // Applies one org.freedesktop.DBus.Properties signal. The properties listed as invalidated are
    // removed from the object. Interfaces that are not kept are ignored.
    void applyPropertiesChanged(const std::string& path,
                                const std::string& interfaceName,
                                const BluetoothProperties& changed,
                                const std::vector<std::string>& invalidated = {});

    // Forgets every object, used when org.bluez leaves the bus.
    void clear();

    // The operation that is in flight for a device and the last failure, both are merged into the
    // snapshot.
    void setPending(const std::string& devicePath, const BluetoothOp op);
    void setLastError(const std::string& error);
    void clearPending(const std::string& devicePath);

    // Builds the snapshot. The classification, the discovery filter and the ordering are applied
    // here, so that the published snapshot is already what the UI shows.
    BluetoothSnapshot toSnapshot(const std::uint64_t sequence) const;

    // Classifies a device from its service UUIDs, its icon and its appearance.
    static BluetoothDeviceKind classify(const BluetoothDevice& device);

    // Turns an error name and message, normally org.bluez.Error.*, into a message for the user.
    static std::string describeError(const std::string& errorName, const std::string& message);

private:
    BluetoothObjectMap mObjects;
    std::map<std::string, BluetoothOp> mPending;
    std::string mLastError;
};

#endif // ES_CORE_BLUETOOTH_STATE_H
