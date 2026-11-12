//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothManagerTests.cpp
//
//  Test cases for the BluetoothManager singleton itself. They also keep the selected backend in
//  the link: until the UI uses the manager nothing else references it, so without these the sd-bus
//  implementation would be compiled but never linked.
//

#include "TestHarness.h"

#include "bluetooth/BluetoothManager.h"

TEST_CASE(bluetoothManagerPublishesASnapshot)
{
    BluetoothManager& manager {BluetoothManager::getInstance()};

    // The snapshot is never null, so that the UI can draw the first frame before the worker has
    // read anything.
    const std::shared_ptr<const BluetoothSnapshot> snapshot {manager.getSnapshot()};
    EXPECT(snapshot != nullptr);

    // The sequence number is a cheap way to detect changes.
    EXPECT(manager.getSequence() >= snapshot->sequence);
}
