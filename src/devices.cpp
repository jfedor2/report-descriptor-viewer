// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <windows.h>

#include <initguid.h>

#include <cfgmgr32.h>
#include <devpkey.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <winioctl.h>
#include <usbioctl.h>
#include <usbiodef.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <map>

#include "devices.h"
#include "util.h"

static std::wstring instance_id(DEVINST devinst) {
    wchar_t buf[MAX_DEVICE_ID_LEN + 1];
    if (CM_Get_Device_IDW(devinst, buf, MAX_DEVICE_ID_LEN + 1, 0) != CR_SUCCESS) {
        return std::wstring();
    }
    return buf;
}

static std::wstring string_property(DEVINST devinst, const DEVPROPKEY& key) {
    DEVPROPTYPE type;
    ULONG size = 0;
    if (CM_Get_DevNode_PropertyW(devinst, &key, &type, nullptr, &size, 0) != CR_BUFFER_SMALL ||
        type != DEVPROP_TYPE_STRING) {
        return std::wstring();
    }
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (CM_Get_DevNode_PropertyW(devinst, &key, &type, (PBYTE) s.data(), &size, 0) != CR_SUCCESS) {
        return std::wstring();
    }
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

static bool uint32_property(DEVINST devinst, const DEVPROPKEY& key, uint32_t& out) {
    DEVPROPTYPE type;
    ULONG size = sizeof(out);
    return CM_Get_DevNode_PropertyW(devinst, &key, &type, (PBYTE) &out, &size, 0) == CR_SUCCESS &&
           type == DEVPROP_TYPE_UINT32;
}

static std::wstring hub_interface_path(DEVINST hub) {
    std::wstring id = instance_id(hub);
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID) &GUID_DEVINTERFACE_USB_HUB, id.data(),
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        len <= 1) {
        return std::wstring();
    }
    std::wstring list(len, L'\0');
    if (CM_Get_Device_Interface_ListW((LPGUID) &GUID_DEVINTERFACE_USB_HUB, id.data(), list.data(), len,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return std::wstring();
    }
    return list.c_str();  // first entry
}

static std::string bus_description(const std::wstring& enumerator) {
    std::wstring e = enumerator;
    std::transform(e.begin(), e.end(), e.begin(), towupper);
    if (e == L"BTHENUM") {
        return "Bluetooth";
    }
    if (e == L"BTHLEDEVICE") {
        return "Bluetooth LE";
    }
    if (e == L"ACPI") {
        return "I2C/ACPI";
    }
    return to_utf8(enumerator);
}

// Fills in what we know about the device from the HID driver: VID/PID,
// strings and the top-level collection.
static void query_hid(const wchar_t* path, HidInterface& dev, HidCollection& collection) {
    // No access rights needed for this and it works for keyboards and mice too.
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    HIDD_ATTRIBUTES attr = { sizeof(attr) };
    if (HidD_GetAttributes(h, &attr)) {
        dev.vid = attr.VendorID;
        dev.pid = attr.ProductID;
        dev.version = attr.VersionNumber;
    }
    wchar_t buf[256] = {};
    if (dev.product.empty() && HidD_GetProductString(h, buf, sizeof(buf) - sizeof(wchar_t))) {
        dev.product = to_utf8(buf);
    }
    buf[0] = 0;
    if (dev.manufacturer.empty() && HidD_GetManufacturerString(h, buf, sizeof(buf) - sizeof(wchar_t))) {
        dev.manufacturer = to_utf8(buf);
    }
    PHIDP_PREPARSED_DATA preparsed;
    if (HidD_GetPreparsedData(h, &preparsed)) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
            collection.usage_page = caps.UsagePage;
            collection.usage = caps.Usage;
        }
        HidD_FreePreparsedData(preparsed);
    }
    CloseHandle(h);
}

// Works out where on the USB tree the HID device's parent node is.
static void locate_usb(DEVINST parent, HidInterface& dev) {
    std::wstring enumerator = string_property(parent, DEVPKEY_Device_EnumeratorName);
    if (_wcsicmp(enumerator.c_str(), L"USB") != 0) {
        dev.not_usb_reason = "Not a USB device (" + bus_description(enumerator) +
                             "). Only USB devices can be asked for their report descriptor.";
        return;
    }

    // Interfaces of composite devices are child nodes named USB\VID_x&PID_y&MI_nn.
    DEVINST usb_device = parent;
    size_t mi = dev.key.find(L"&MI_");
    if (mi != std::wstring::npos) {
        dev.interface_number = (int) wcstoul(dev.key.c_str() + mi + 4, nullptr, 16);
        if (CM_Get_Parent(&usb_device, parent, 0) != CR_SUCCESS) {
            dev.not_usb_reason = "Couldn't find the parent USB device.";
            return;
        }
    }
    dev.usb_device_id = instance_id(usb_device);
    if (!uint32_property(usb_device, DEVPKEY_Device_Address, dev.port)) {
        dev.not_usb_reason = "Couldn't find the USB port number.";
        return;
    }
    DEVINST hub;
    if (CM_Get_Parent(&hub, usb_device, 0) != CR_SUCCESS) {
        dev.not_usb_reason = "Couldn't find the USB hub.";
        return;
    }
    dev.hub_path = hub_interface_path(hub);
    if (dev.hub_path.empty()) {
        dev.not_usb_reason = "Couldn't open the USB hub.";
        return;
    }
    dev.usb = true;
}

std::vector<HidInterface> list_hid_interfaces() {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<HidInterface> result;
    std::map<std::wstring, size_t> by_key;
    SP_DEVICE_INTERFACE_DATA iface = { sizeof(iface) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, i, &iface); i++) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        if (size == 0) {
            continue;
        }
        std::vector<uint8_t> buf(size);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*) buf.data();
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info = { sizeof(info) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, &info)) {
            continue;
        }
        DEVINST parent;
        if (CM_Get_Parent(&parent, info.DevInst, 0) != CR_SUCCESS) {
            continue;
        }

        // All top-level collections of one interface share a parent node.
        std::wstring key = instance_id(parent);
        auto it = by_key.find(key);
        bool is_new = it == by_key.end();
        if (is_new) {
            it = by_key.emplace(key, result.size()).first;
            result.emplace_back();
            HidInterface& dev = result.back();
            dev.key = key;
            dev.product = to_utf8(string_property(parent, DEVPKEY_Device_BusReportedDeviceDesc));
            locate_usb(parent, dev);
        }
        HidInterface& dev = result[it->second];
        dev.hid_instance_ids.push_back(instance_id(info.DevInst));
        HidCollection collection;
        query_hid(detail->DevicePath, dev, collection);
        dev.collections.push_back(collection);
        if (is_new && dev.product.empty()) {
            dev.product = to_utf8(string_property(parent, DEVPKEY_Device_DeviceDesc));
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    std::sort(result.begin(), result.end(), [](const HidInterface& a, const HidInterface& b) {
        if (a.usb != b.usb) {
            return a.usb;
        }
        if (a.vid != b.vid) {
            return a.vid < b.vid;
        }
        if (a.pid != b.pid) {
            return a.pid < b.pid;
        }
        return a.interface_number < b.interface_number;
    });
    return result;
}

// ---------------------------------------------------------------------------

static std::string last_error_message() {
    DWORD err = GetLastError();
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (LPSTR) &msg, 0, nullptr);
    std::string s = msg ? msg : "";
    LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) {
        s.pop_back();
    }
    char code[32];
    snprintf(code, sizeof(code), " (error %lu)", err);
    return s + code;
}

// Sends a GET_DESCRIPTOR control request to the device on the given hub port.
// The hub driver always sends it with bmRequestType 0x80 (recipient: device),
// whatever we put in the request.
static bool get_descriptor(HANDLE hub, uint32_t port, uint16_t value, uint16_t index,
    uint16_t length, std::vector<uint8_t>& out, std::string& error) {
    const size_t header = offsetof(USB_DESCRIPTOR_REQUEST, Data);
    std::vector<uint8_t> buf(header + length);
    auto* req = (USB_DESCRIPTOR_REQUEST*) buf.data();
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = 0x80;
    req->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    req->SetupPacket.wValue = value;
    req->SetupPacket.wIndex = index;
    req->SetupPacket.wLength = length;
    DWORD returned = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf.data(), (DWORD) buf.size(),
            buf.data(), (DWORD) buf.size(), &returned, nullptr)) {
        error = last_error_message();
        return false;
    }
    if (returned < header) {
        error = "Short response from the hub driver.";
        return false;
    }
    out.assign(buf.begin() + header, buf.begin() + returned);
    return true;
}

bool find_report_descriptor(const std::vector<uint8_t>& config, int& interface_number, uint16_t& length) {
    int current = -1;
    bool current_is_target = false;
    for (size_t i = 0; i + 2 <= config.size() && config[i] >= 2; i += config[i]) {
        const uint8_t* d = &config[i];
        const size_t len = std::min<size_t>(d[0], config.size() - i);
        if (d[1] == USB_INTERFACE_DESCRIPTOR_TYPE && len >= 9) {
            current = d[2];
            const bool alt0 = d[3] == 0;
            current_is_target = alt0 && (interface_number < 0 ? d[5] == 3 : current == interface_number);
        } else if (d[1] == 0x21 && current_is_target && len >= 6) {
            const int count = d[5];
            for (int k = 0; k < count && 6 + 3 * k + 3 <= (int) len; k++) {
                if (d[6 + 3 * k] == 0x22) {
                    interface_number = current;
                    length = (uint16_t) (d[7 + 3 * k] | (d[8 + 3 * k] << 8));
                    return true;
                }
            }
        }
    }
    return false;
}

bool fetch_report_descriptor(const HidInterface& dev, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    if (!dev.usb) {
        error = dev.not_usb_reason;
        return false;
    }
    HANDLE hub = CreateFileW(dev.hub_path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hub == INVALID_HANDLE_VALUE) {
        error = "Couldn't open the USB hub: " + last_error_message();
        return false;
    }

    bool ok = false;
    std::vector<uint8_t> config;
    int interface_number = dev.interface_number;
    uint16_t length = 0;
    std::string err;
    if (get_descriptor(hub, dev.port, USB_CONFIGURATION_DESCRIPTOR_TYPE << 8, 0, 9, config, err) &&
        config.size() >= 4) {
        const uint16_t total = (uint16_t) (config[2] | (config[3] << 8));
        if (!get_descriptor(hub, dev.port, USB_CONFIGURATION_DESCRIPTOR_TYPE << 8, 0, total, config, err)) {
            config.clear();
        }
    }
    if (!find_report_descriptor(config, interface_number, length)) {
        // Couldn't tell the length; ask for the most and see what we get.
        length = 4096;
        if (interface_number < 0) {
            interface_number = 0;
        }
    }

    if (length == 0) {
        error = "The HID descriptor says the report descriptor is empty.";
    } else if (get_descriptor(hub, dev.port, 0x22 << 8, (uint16_t) interface_number, length, out, err)) {
        // HID report descriptors should be requested with the interface as
        // the recipient. We can only ask with the device as the recipient,
        // which a lot of devices accept anyway, but not all.
        if (out.empty()) {
            error = "The device returned an empty report descriptor.";
        } else {
            ok = true;
        }
    } else {
        error = "GET_DESCRIPTOR request failed: " + err +
                ".\n\nThe Windows USB hub driver only lets us send this request addressed to the device, not "
                "the interface. This device doesn't accept it that way.";
    }
    CloseHandle(hub);
    return ok;
}

std::vector<std::pair<uint16_t, uint16_t>> expected_top_level(const HidInterface& dev) {
    std::vector<std::pair<uint16_t, uint16_t>> result;
    for (const HidCollection& c : dev.collections) {
        result.emplace_back(c.usage_page, c.usage);
    }
    return result;
}

const char* const MISMATCH_WARNING =
    "The top-level collections in this descriptor don't match the ones Windows reports for this interface. "
    "The device may have answered with the descriptor of a different interface.";
