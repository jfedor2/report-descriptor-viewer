// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <windows.h>

#include <initguid.h>

#include <cfgmgr32.h>

#include <devpkey.h>
#include <newdev.h>
#include <setupapi.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winusb.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "devices.h"
#include "util.h"
#include "winusb.h"

// The device interface WinUSB registers for the device while we have it.
// {6F1D8C1A-3B52-4E0B-9C51-2D8A7E4F9B30}
DEFINE_GUID(GUID_RDV_WINUSB_INTERFACE, 0x6f1d8c1a, 0x3b52, 0x4e0b, 0x9c, 0x51, 0x2d, 0x8a, 0x7e, 0x4f, 0x9b, 0x30);
static const wchar_t* RDV_WINUSB_INTERFACE = L"{6F1D8C1A-3B52-4E0B-9C51-2D8A7E4F9B30}";

// The class winusb.inf installs devices in ("USBDevice").
DEFINE_GUID(GUID_CLASS_USBDEVICE, 0x88bae032, 0x5a81, 0x49f0, 0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6);

static std::string error_message(DWORD err) {
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (LPSTR) &msg, 0, nullptr);
    std::string s = msg ? msg : "";
    LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) {
        s.pop_back();
    }
    char code[32];
    snprintf(code, sizeof(code), " (error 0x%lX)", err);
    return s + code;
}

static std::string failed(const char* what) {
    return std::string(what) + ": " + error_message(GetLastError());
}

// ---------------------------------------------------------------------------
// The marker file that says which device needs its driver put back.

static std::wstring marker_path() {
    wchar_t* dir = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &dir))) {
        path = std::wstring(dir) + L"\\report-descriptor-viewer";
    }
    CoTaskMemFree(dir);
    return path.empty() ? path : path + L"\\pending-restore.txt";
}

struct Pending {
    std::wstring id;   // device instance ID
    std::wstring inf;  // the INF its original driver came from, like "input.inf"
};

static bool write_marker(const Pending& p) {
    std::wstring path = marker_path();
    CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << to_utf8(p.id) << "\n" << to_utf8(p.inf) << "\n";
    return f.good();
}

static bool read_marker(Pending& p) {
    std::ifstream f(marker_path(), std::ios::binary);
    std::string id, inf;
    if (!std::getline(f, id) || id.empty()) {
        return false;
    }
    std::getline(f, inf);
    p.id = from_utf8(id);
    p.inf = from_utf8(inf);
    return true;
}

bool restore_pending(std::wstring* usb_node_id) {
    Pending p;
    if (!read_marker(p)) {
        return false;
    }
    if (usb_node_id) {
        *usb_node_id = p.id;
    }
    return true;
}

// ---------------------------------------------------------------------------

// A device information set holding just the one device.
struct DeviceSet {
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev = { sizeof(SP_DEVINFO_DATA) };

    bool open(const std::wstring& id, std::string& error) {
        set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
        if (set == INVALID_HANDLE_VALUE) {
            error = failed("SetupDiCreateDeviceInfoList");
            return false;
        }
        if (!SetupDiOpenDeviceInfoW(set, id.c_str(), nullptr, 0, &dev)) {
            error = failed("SetupDiOpenDeviceInfo");
            return false;
        }
        return true;
    }

    ~DeviceSet() {
        if (set != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
};

static std::wstring string_property(const DeviceSet& d, const DEVPROPKEY& key) {
    DEVPROPTYPE type;
    wchar_t buf[512] = {};
    if (!SetupDiGetDevicePropertyW(d.set, (PSP_DEVINFO_DATA) &d.dev, &key, &type, (PBYTE) buf, sizeof(buf) - sizeof(wchar_t),
            nullptr, 0) ||
        type != DEVPROP_TYPE_STRING) {
        return std::wstring();
    }
    return buf;
}

static std::wstring file_name(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

// Installs the given driver node, or the best compatible one if there's no match.
static bool install_driver(DeviceSet& d, DWORD type, const std::wstring& inf_name, const wchar_t* hardware_id,
    std::string& error) {
    if (!SetupDiBuildDriverInfoList(d.set, &d.dev, type)) {
        error = failed("SetupDiBuildDriverInfoList");
        return false;
    }
    SP_DRVINFO_DATA_W drv = { sizeof(drv) };
    bool found = false;
    std::string seen;
    for (DWORD i = 0; !found && SetupDiEnumDriverInfoW(d.set, &d.dev, type, i, &drv); i++) {
        DWORD size = 0;
        SetupDiGetDriverInfoDetailW(d.set, &d.dev, &drv, nullptr, 0, &size);
        std::vector<uint8_t> buf(std::max<DWORD>(size, sizeof(SP_DRVINFO_DETAIL_DATA_W)));
        auto* detail = (SP_DRVINFO_DETAIL_DATA_W*) buf.data();
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDriverInfoDetailW(d.set, &d.dev, &drv, detail, (DWORD) buf.size(), nullptr)) {
            continue;
        }
        seen += " [" + to_utf8(file_name(detail->InfFileName)) + " " + to_utf8(detail->HardwareID) + "]";
        found = _wcsicmp(file_name(detail->InfFileName).c_str(), inf_name.c_str()) == 0 &&
                (hardware_id == nullptr || _wcsicmp(detail->HardwareID, hardware_id) == 0);
    }
    if (!found) {
        if (type != SPDIT_COMPATDRIVER) {
            error = "Couldn't find the driver to install. Found:" + (seen.empty() ? std::string(" nothing") : seen);
            return false;
        }
        // Let Windows pick, the way it would for a newly connected device.
        if (!SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, d.set, &d.dev) ||
            !SetupDiGetSelectedDriverW(d.set, &d.dev, &drv)) {
            error = failed("Selecting the best driver");
            return false;
        }
    }
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, d.set, &d.dev, &drv, DIIDFLAG_NOFINISHINSTALLUI, &reboot)) {
        error = failed("DiInstallDevice");
        return false;
    }
    return true;
}

static bool set_interface_guid(DeviceSet& d, bool set) {
    HKEY key = SetupDiCreateDevRegKeyW(d.set, &d.dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (key == INVALID_HANDLE_VALUE) {
        return false;
    }
    LSTATUS status;
    if (set) {
        std::wstring multi_sz = std::wstring(RDV_WINUSB_INTERFACE) + L'\0';
        status = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE*) multi_sz.c_str(),
            (DWORD) ((multi_sz.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"DeviceInterfaceGUIDs");
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS || (!set && status == ERROR_FILE_NOT_FOUND);
}

static bool switch_to_winusb(const std::wstring& id, std::string& error) {
    {
        DeviceSet d;
        if (!d.open(id, error)) {
            return false;
        }
        if (!set_interface_guid(d, true)) {
            error = "Couldn't set the device interface GUID for WinUSB.";
            return false;
        }
        // winusb.inf is for the USBDevice class, and drivers from other
        // classes aren't even considered unless the device is in that class.
        wchar_t class_guid[64];
        StringFromGUID2(GUID_CLASS_USBDEVICE, class_guid, 64);
        if (!SetupDiSetDeviceRegistryPropertyW(d.set, &d.dev, SPDRP_CLASSGUID, (const BYTE*) class_guid,
                (DWORD) ((wcslen(class_guid) + 1) * sizeof(wchar_t)))) {
            error = failed("Changing the device class");
            return false;
        }
    }
    // Open it again so SetupAPI sees the new class.
    DeviceSet d;
    if (!d.open(id, error)) {
        return false;
    }
    if (d.dev.ClassGuid != GUID_CLASS_USBDEVICE) {
        error = "Changing the device class didn't work.";
        return false;
    }
    SP_DEVINSTALL_PARAMS_W params = { sizeof(params) };
    SetupDiGetDeviceInstallParamsW(d.set, &d.dev, &params);
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;  // otherwise the list comes out empty
    GetWindowsDirectoryW(params.DriverPath, MAX_PATH);
    wcscat_s(params.DriverPath, L"\\INF\\winusb.inf");
    if (!SetupDiSetDeviceInstallParamsW(d.set, &d.dev, &params)) {
        error = failed("SetupDiSetDeviceInstallParams");
        return false;
    }
    return install_driver(d, SPDIT_CLASSDRIVER, L"winusb.inf", L"USB\\MS_COMP_WINUSB", error);
}

static bool restore_driver(const Pending& p, std::string& error) {
    {
        DeviceSet d;
        if (d.open(p.id, error) && install_driver(d, SPDIT_COMPATDRIVER, p.inf, nullptr, error)) {
            set_interface_guid(d, false);
            return true;
        }
    }
    // Plan B: remove the device and have Windows find it again, which
    // installs the driver it would normally pick.
    std::string first_error = error;
    DeviceSet d;
    if (!d.open(p.id, error)) {
        return false;
    }
    DEVINST parent = 0;
    CM_Get_Parent(&parent, d.dev.DevInst, 0);
    BOOL reboot = FALSE;
    if (!DiUninstallDevice(nullptr, d.set, &d.dev, 0, &reboot)) {
        error = first_error + "; " + failed("DiUninstallDevice");
        return false;
    }
    if (parent != 0) {
        CM_Reenumerate_DevNode(parent, CM_REENUMERATE_SYNCHRONOUS);
    }
    return true;
}

bool restore_now(std::string& error) {
    Pending p;
    if (!read_marker(p)) {
        return true;
    }
    if (!restore_driver(p, error)) {
        return false;
    }
    DeleteFileW(marker_path().c_str());
    return true;
}

// Waits for WinUSB to come up and register our device interface.
static std::wstring wait_for_interface(const std::wstring& id) {
    for (int i = 0; i < 50; i++) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID) &GUID_RDV_WINUSB_INTERFACE, (DEVINSTID_W) id.c_str(),
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
            len > 1) {
            std::wstring list(len, L'\0');
            if (CM_Get_Device_Interface_ListW((LPGUID) &GUID_RDV_WINUSB_INTERFACE, (DEVINSTID_W) id.c_str(),
                    list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS) {
                return list.c_str();
            }
        }
        Sleep(100);
    }
    return std::wstring();
}

static bool read_with_winusb(const std::wstring& id, int interface_number, std::vector<uint8_t>& out, std::string& error) {
    std::wstring path = wait_for_interface(id);
    if (path.empty()) {
        error = "WinUSB didn't start for the device.";
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = failed("Opening the device with WinUSB");
        return false;
    }
    WINUSB_INTERFACE_HANDLE usb;
    if (!WinUsb_Initialize(file, &usb)) {
        error = failed("WinUsb_Initialize");
        CloseHandle(file);
        return false;
    }

    bool ok = false;
    uint16_t length = 0;
    std::vector<uint8_t> config(9);
    ULONG transferred = 0;
    if (WinUsb_GetDescriptor(usb, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, config.data(), (ULONG) config.size(),
            &transferred) &&
        transferred >= 4) {
        config.resize(config[2] | (config[3] << 8));
        if (WinUsb_GetDescriptor(usb, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, config.data(), (ULONG) config.size(),
                &transferred)) {
            config.resize(transferred);
        } else {
            config.clear();
        }
    } else {
        config.clear();
    }
    if (!find_report_descriptor(config, interface_number, length)) {
        length = 4096;
        if (interface_number < 0) {
            interface_number = 0;
        }
    }

    out.resize(length);
    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = 0x81;  // device-to-host, standard, interface
    setup.Request = USB_REQUEST_GET_DESCRIPTOR;
    setup.Value = 0x22 << 8;
    setup.Index = (USHORT) interface_number;
    setup.Length = length;
    if (WinUsb_ControlTransfer(usb, setup, out.data(), length, &transferred, nullptr)) {
        out.resize(transferred);
        ok = !out.empty();
        if (!ok) {
            error = "The device returned an empty report descriptor.";
        }
    } else {
        error = failed("GET_DESCRIPTOR request");
        out.clear();
    }
    WinUsb_Free(usb);
    CloseHandle(file);
    return ok;
}

bool fetch_report_descriptor_winusb(
    const std::wstring& usb_node_id, int interface_number, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    std::string restore_error;
    if (!restore_now(restore_error)) {
        error = "A device from an earlier attempt is still on WinUSB and couldn't be restored: " + restore_error;
        return false;
    }

    Pending p;
    p.id = usb_node_id;
    {
        DeviceSet d;
        if (!d.open(usb_node_id, error)) {
            return false;
        }
        p.inf = string_property(d, DEVPKEY_Device_DriverInfPath);
    }
    if (p.inf.empty()) {
        error = "Couldn't tell which driver the device uses now.";
        return false;
    }
    if (_wcsicmp(p.inf.c_str(), L"winusb.inf") == 0) {
        error = "The device is already using WinUSB.";
        return false;
    }
    if (!write_marker(p)) {
        error = "Couldn't write the restore marker file (are we running as administrator?).";
        return false;
    }

    bool ok = switch_to_winusb(usb_node_id, error) && read_with_winusb(usb_node_id, interface_number, out, error);
    if (!restore_now(restore_error)) {
        error = (ok ? std::string() : error + "\n\n") + "Putting the original driver back failed: " + restore_error;
        return false;
    }
    return ok;
}
