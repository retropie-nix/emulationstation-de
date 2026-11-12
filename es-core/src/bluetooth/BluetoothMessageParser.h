//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  BluetoothMessageParser.h
//
//  Parsing of the sd-bus messages that BlueZ sends and replies with.
//
//  The parsers are separate from the manager that uses them so that they can be exercised with a
//  hand built message instead of a live bus.
//

#ifndef ES_CORE_BLUETOOTH_MESSAGE_PARSER_H
#define ES_CORE_BLUETOOTH_MESSAGE_PARSER_H

#include "bluetooth/BluetoothState.h"

#include <systemd/sd-bus.h>

namespace BluetoothMessageParser
{
    // Reads an a{sv} dictionary, unwrapping the variant that every value is carried in and
    // skipping the values whose type cannot be reduced.
    bool readProperties(sd_bus_message* message, BluetoothProperties& properties);

    // Reads an a{sa{sv}} dictionary, keeping the interfaces that BluetoothState reduces.
    bool readInterfaces(sd_bus_message* message, BluetoothInterfaces& interfaces);

    // Reads the a{oa{sa{sv}}} reply of org.freedesktop.DBus.ObjectManager.GetManagedObjects.
    bool readManagedObjects(sd_bus_message* message, BluetoothObjectMap& objects);

} // namespace BluetoothMessageParser

#endif // ES_CORE_BLUETOOTH_MESSAGE_PARSER_H
