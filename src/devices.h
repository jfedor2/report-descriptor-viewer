#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// A top-level collection as Windows reports it for a HID device.
struct HidCollection {
    uint16_t usage_page = 0;
    uint16_t usage = 0;
};

// One HID interface. A USB interface with several top-level collections shows
// up in Windows as several HID devices; they are merged into one entry here
// because they share a single report descriptor.
struct HidInterface {
    std::wstring key;  // instance ID of the node the descriptor belongs to
    std::string product;
    std::string manufacturer;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t version = 0;
    std::vector<HidCollection> collections;
    std::vector<std::wstring> hid_instance_ids;

    // Only set for USB devices, which are the only ones we can fetch from.
    bool usb = false;
    std::wstring usb_device_id;  // instance ID of the USB device node
    int interface_number = -1;   // -1: not known, use the first HID interface
    std::wstring hub_path;
    uint32_t port = 0;
    std::string not_usb_reason;
};

std::vector<HidInterface> list_hid_interfaces();

// Asks the device for its HID report descriptor through the USB hub driver.
bool fetch_report_descriptor(const HidInterface& dev, std::vector<uint8_t>& out, std::string& error);

// Finds the HID descriptor of the given interface (or of the first HID
// interface if it's -1) in a configuration descriptor and returns the
// interface number and report descriptor length from it.
bool find_report_descriptor(const std::vector<uint8_t>& config, int& interface_number, uint16_t& length);

// Usage page and usage of each top-level collection, to compare with what the
// fetched descriptor says.
std::vector<std::pair<uint16_t, uint16_t>> expected_top_level(const HidInterface& dev);

extern const char* const MISMATCH_WARNING;
