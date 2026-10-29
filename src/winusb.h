#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Fetching the report descriptor by temporarily switching the device (or
// interface) to the WinUSB driver, which lets us send the request to the
// interface like we're supposed to. The original driver is put back
// afterwards. Needs administrator rights.
//
// usb_node_id is the instance ID of the USB node the HID driver sits on
// (HidInterface::key).
bool fetch_report_descriptor_winusb(
    const std::wstring& usb_node_id, int interface_number, std::vector<uint8_t>& out, std::string& error);

// If we crashed or were killed while a device was switched to WinUSB, a
// marker file is left behind. These check for it and put the original
// driver back (the latter needs administrator rights).
bool restore_pending(std::wstring* usb_node_id);
bool restore_now(std::string& error);
