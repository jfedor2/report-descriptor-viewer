// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Command line version: lists HID devices and prints their report descriptors.
//
//   rdv-dump           list devices
//   rdv-dump N         decode the report descriptor of device N
//   rdv-dump --all     decode them all
//   rdv-dump --winusb N
//                      fetch it by switching device N to WinUSB for a moment
//                      (needs administrator rights)
//   rdv-dump --restore put back the driver of a device left on WinUSB

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "decoder.h"
#include "devices.h"
#include "usages.h"
#include "util.h"
#include "winusb.h"

static void print_device(size_t index, const HidInterface& dev) {
    printf("[%zu] %04X:%04X", index, dev.vid, dev.pid);
    if (dev.interface_number >= 0) {
        printf(" interface %d", dev.interface_number);
    }
    printf("  %s", dev.product.c_str());
    if (!dev.manufacturer.empty()) {
        printf(" (%s)", dev.manufacturer.c_str());
    }
    printf("\n");
    for (const HidCollection& c : dev.collections) {
        std::string usage = usage_name(c.usage_page, c.usage);
        printf("      %s / %s\n", usage_page_name(c.usage_page).c_str(),
            usage.empty() ? ("0x" + std::to_string(c.usage)).c_str() : usage.c_str());
    }
    printf("      %s\n", to_utf8(dev.key).c_str());
    if (!dev.usb) {
        printf("      %s\n", dev.not_usb_reason.c_str());
    }
}

static int dump(size_t index, const HidInterface& dev, bool winusb = false) {
    print_device(index, dev);
    std::vector<uint8_t> data;
    std::string error;
    const bool ok = winusb ? fetch_report_descriptor_winusb(dev.key, dev.interface_number, data, error)
                           : fetch_report_descriptor(dev, data, error);
    if (!ok) {
        printf("\n  %s\n\n", error.c_str());
        return 1;
    }
    DecodedDescriptor decoded = decode_report_descriptor(data);
    printf("\n%s\n", format_decoded(decoded, data).c_str());
    for (const std::string& line : format_reports(decoded)) {
        printf("// %s\n", line.c_str());
    }
    for (const std::string& p : decoded.problems) {
        printf("// Problem: %s\n", p.c_str());
    }
    printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<HidInterface> devices = list_hid_interfaces();

    if (argc < 2) {
        for (size_t i = 0; i < devices.size(); i++) {
            print_device(i, devices[i]);
        }
        return 0;
    }
    if (strcmp(argv[1], "--all") == 0) {
        int failed = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            failed += dump(i, devices[i]);
        }
        return failed ? 1 : 0;
    }
    if (strcmp(argv[1], "--restore") == 0) {
        std::string error;
        if (!restore_now(error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    const bool winusb = strcmp(argv[1], "--winusb") == 0;
    size_t index = strtoul(argv[winusb && argc > 2 ? 2 : 1], nullptr, 10);
    if (index >= devices.size()) {
        fprintf(stderr, "No device %zu.\n", index);
        return 1;
    }
    return dump(index, devices[index], winusb);
}
