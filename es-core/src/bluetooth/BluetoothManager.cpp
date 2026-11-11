//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothManager.cpp
//
//  Selects the Bluetooth device management implementation and provides the null implementation
//  that is used on every platform without a backend.
//

#include "bluetooth/BluetoothManager.h"

#if defined(BLUETOOTH_MANAGER_BLUEZ)
// Defined by BluetoothManagerBlueZ.cpp.
std::unique_ptr<BluetoothManager> createBlueZBluetoothManager();
#endif

namespace
{
    // Reports the service as unavailable and ignores all actions. This is what every platform
    // without a backend, and every build without BLUETOOTH_MANAGER, gets.
    class NullBluetoothManager : public BluetoothManager
    {
    public:
        NullBluetoothManager()
            : mSnapshot {std::make_shared<BluetoothSnapshot>()}
        {
        }

        bool isAvailable() const override { return false; }
        std::shared_ptr<const BluetoothSnapshot> getSnapshot() const override { return mSnapshot; }
        std::uint64_t getSequence() const override { return mSnapshot->sequence; }

        void setPowered(const bool) override { }
        void startDiscovery() override { }
        void stopDiscovery() override { }
        void pair(const std::string&) override { }
        void connect(const std::string&) override { }
        void disconnect(const std::string&) override { }
        void forget(const std::string&) override { }

    private:
        std::shared_ptr<const BluetoothSnapshot> mSnapshot;
    };

    // The backend is chosen here, at build time.
    std::unique_ptr<BluetoothManager> createBluetoothManager()
    {
#if defined(BLUETOOTH_MANAGER_BLUEZ)
        return createBlueZBluetoothManager();
#else
        return std::make_unique<NullBluetoothManager>();
#endif
    }

} // namespace

BluetoothManager& BluetoothManager::getInstance()
{
    static const std::unique_ptr<BluetoothManager> instance {createBluetoothManager()};
    return *instance;
}
