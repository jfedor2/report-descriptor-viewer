// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <windows.h>

#include <commdlg.h>
#include <d3d11.h>
#include <dbt.h>
#include <hidsdi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "decoder.h"
#include "devices.h"
#include "usages.h"
#include "util.h"
#include "winusb.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_device_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static bool g_swap_chain_occluded = false;
static UINT g_resize_width = 0;
static UINT g_resize_height = 0;
static float g_pending_dpi_scale = 0.0f;

static const ImVec4 COLOR_ERROR(1.0f, 0.42f, 0.42f, 1.0f);
static const ImVec4 COLOR_OK(0.45f, 0.9f, 0.55f, 1.0f);
static const ImVec4 COLOR_WARN(1.0f, 0.8f, 0.35f, 1.0f);
static const ImVec4 COLOR_DIM(0.55f, 0.55f, 0.6f, 1.0f);

// Decoded item colors.
static const ImVec4 COLOR_MAIN(0.5f, 0.75f, 1.0f, 1.0f);
static const ImVec4 COLOR_GLOBAL(0.92f, 0.92f, 0.92f, 1.0f);
static const ImVec4 COLOR_LOCAL(0.6f, 0.88f, 0.6f, 1.0f);

// The last descriptor we got from a device.
struct CachedDescriptor {
    std::vector<uint8_t> data;
    bool via_winusb = false;
    std::string time;  // when we got it, like "14:03:59"
};

// Application state, owned by the UI thread.
struct App {
    HWND main_hwnd = nullptr;
    ImFont* mono_font = nullptr;

    std::vector<HidInterface> devices;
    bool show_non_usb = true;
    ULONGLONG refresh_at = 0;  // pending refresh after a device change, 0 = none

    // The selected device and what we got from it. The device is kept even
    // if it disappears, which it does for a moment while it's on WinUSB.
    std::wstring selected_key;
    HidInterface selected_dev;
    bool connected = false;
    bool fetched = false;
    bool via_winusb = false;
    std::string fetched_at;
    std::vector<uint8_t> descriptor;
    std::string fetch_error;
    DecodedDescriptor decoded;
    std::vector<std::string> report_lines;
    bool collections_match = true;

    // The elevated copy of ourselves doing the WinUSB switch.
    HANDLE helper = nullptr;
    bool helper_restoring = false;
    std::wstring helper_key;
    std::wstring helper_out;
    std::string helper_error;  // for the restore banner

    // Set if a device was left on WinUSB.
    bool restore_needed = false;
    std::wstring restore_id;

    // Descriptors we already have, by device instance ID, so we don't have
    // to ask again (which is a hassle with WinUSB) when going back to one.
    std::map<std::wstring, CachedDescriptor> cache;

    const HidInterface* find(const std::wstring& key) const {
        for (const HidInterface& dev : devices) {
            if (dev.key == key) {
                return &dev;
            }
        }
        return nullptr;
    }

    void refresh() {
        devices = list_hid_interfaces();
        const HidInterface* dev = find(selected_key);
        connected = dev != nullptr;
        if (dev) {
            selected_dev = *dev;
        }
        restore_needed = restore_pending(&restore_id);
    }

    void select(const std::wstring& key) {
        selected_key = key;
        connected = false;
        fetched = false;
        const HidInterface* dev = find(key);
        if (dev) {
            selected_dev = *dev;
            connected = true;
        }
        if (cache.count(key) || !dev) {
            show(std::string());
        } else {
            fetch();
        }
    }

    void fetch() {
        std::vector<uint8_t> data;
        std::string error;
        fetch_report_descriptor(selected_dev, data, error);
        set_result(false, data, error);
    }

    void store(const std::wstring& key, bool winusb, const std::vector<uint8_t>& data) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char time[16];
        snprintf(time, sizeof(time), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
        cache[key] = CachedDescriptor{ data, winusb, time };
    }

    // A fetch on the selected device finished. If it failed but we got the
    // descriptor before, we keep showing that one.
    void set_result(bool winusb, const std::vector<uint8_t>& data, const std::string& error) {
        fetched = true;
        if (!data.empty()) {
            store(selected_key, winusb, data);
            show(error);
        } else if (cache.count(selected_key)) {
            show("Fetching it again didn't work, this is from earlier. " + error);
        } else {
            show(error);
        }
    }

    // Shows what we have in the cache for the selected device, plus an error.
    void show(const std::string& error) {
        auto it = cache.find(selected_key);
        if (it != cache.end()) {
            fetched = true;
            descriptor = it->second.data;
            via_winusb = it->second.via_winusb;
            fetched_at = it->second.time;
        } else {
            descriptor.clear();
            via_winusb = false;
            fetched_at.clear();
        }
        fetch_error = error;
        decoded = decode_report_descriptor(descriptor);
        report_lines = format_reports(decoded);
        collections_match = descriptor.empty() || same_top_level(decoded, expected_top_level(selected_dev));
    }
};

static App g_app;

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example_win32_directx11)

static void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_render_target_view != nullptr) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------

static void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

static std::string collection_text(const HidCollection& c) {
    std::string usage = usage_name(c.usage_page, c.usage);
    if (usage.empty()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%02X", c.usage);
        usage = buf;
    }
    return usage_page_name(c.usage_page) + " / " + usage;
}

static void save_descriptor(const HidInterface& dev, const std::vector<uint8_t>& data) {
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%04x_%04x_if%d.bin", dev.vid, dev.pid, std::max(dev.interface_number, 0));
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_app.main_hwnd;
    ofn.lpstrFilter = L"Binary files (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bin";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (f == nullptr || fwrite(data.data(), 1, data.size(), f) != data.size()) {
        MessageBoxW(g_app.main_hwnd, L"Couldn't write the file.", L"Report Descriptor Viewer", MB_ICONERROR);
    }
    if (f != nullptr) {
        fclose(f);
    }
}

// ---------------------------------------------------------------------------
// Switching to WinUSB needs administrator rights, so we run another copy of
// ourselves elevated to do it and wait for it to finish.

static std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

static bool read_file(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) {
        return false;
    }
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return true;
}

static bool write_file(const std::wstring& path, const void* data, size_t size) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) {
        return false;
    }
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

static void start_helper(bool restoring, const std::wstring& args) {
    App& app = g_app;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    app.helper_out = std::wstring(temp) + L"rdv-" + std::to_wstring(GetCurrentProcessId()) + L".bin";
    DeleteFileW(app.helper_out.c_str());
    DeleteFileW((app.helper_out + L".err").c_str());

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring params = args + L" " + quote(app.helper_out);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = app.main_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    app.helper_restoring = restoring;
    app.helper_error.clear();
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        std::string error = GetLastError() == ERROR_CANCELLED ? "Cancelled." : "Couldn't start the elevated helper.";
        if (restoring) {
            app.helper_error = error;
        } else {
            app.set_result(true, std::vector<uint8_t>(), error);
        }
        return;
    }
    app.helper = sei.hProcess;
}

static void start_winusb_fetch() {
    App& app = g_app;
    app.helper_key = app.selected_key;
    start_helper(false, L"--winusb-fetch " + quote(app.helper_key) + L" " +
                            std::to_wstring(app.selected_dev.interface_number));
}

static void start_restore() {
    start_helper(true, L"--winusb-restore");
}

static void finish_helper() {
    App& app = g_app;
    CloseHandle(app.helper);
    app.helper = nullptr;
    std::string data, error;
    read_file(app.helper_out, data);
    if (!read_file(app.helper_out + L".err", error) && data.empty()) {
        error = "The helper didn't say what happened.";
    }
    DeleteFileW(app.helper_out.c_str());
    DeleteFileW((app.helper_out + L".err").c_str());
    if (app.helper_restoring) {
        app.helper_error = error;
    } else if (app.helper_key == app.selected_key) {
        // If we got the descriptor, an error means the restore failed.
        app.set_result(true, std::vector<uint8_t>(data.begin(), data.end()), error);
    } else if (!data.empty()) {
        // They've moved on to another device in the meantime.
        app.store(app.helper_key, true, std::vector<uint8_t>(data.begin(), data.end()));
    }
    app.refresh();
}

// The elevated side. Writes the descriptor to out_path, or what went wrong
// to out_path + ".err".
static int run_helper(int argc, wchar_t** argv) {
    const std::wstring mode = argv[1];
    std::vector<uint8_t> data;
    std::string error;
    bool ok;
    std::wstring out_path;
    if (mode == L"--winusb-fetch" && argc == 5) {
        out_path = argv[4];
        ok = fetch_report_descriptor_winusb(argv[2], _wtoi(argv[3]), data, error);
    } else if (mode == L"--winusb-restore" && argc == 3) {
        out_path = argv[2];
        ok = restore_now(error);
    } else {
        return 2;
    }
    if (!data.empty()) {
        write_file(out_path, data.data(), data.size());
    }
    if (!error.empty()) {
        write_file(out_path + L".err", error.data(), error.size());
    }
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------

static void draw_device_list() {
    App& app = g_app;
    const float em = ImGui::GetFontSize();

    if (ImGui::Button("Refresh")) {
        app.refresh();
    }
    ImGui::SameLine(0, em);
    ImGui::Checkbox("Show non-USB", &app.show_non_usb);
    ImGui::SetItemTooltip("Bluetooth, I2C and virtual HID devices are listed, but we can't ask them\n"
                          "for their report descriptor.");

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("devices", 3, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("VID:PID");
    ImGui::TableSetupColumn("If");
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const HidInterface& dev : app.devices) {
        if (!dev.usb && !app.show_non_usb) {
            continue;
        }
        ImGui::PushID(to_utf8(dev.key).c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (!dev.usb) {
            ImGui::PushStyleColor(ImGuiCol_Text, COLOR_DIM);
        }
        char label[32];
        snprintf(label, sizeof(label), "%04X:%04X", dev.vid, dev.pid);
        if (ImGui::Selectable(label, dev.key == app.selected_key, ImGuiSelectableFlags_SpanAllColumns)) {
            app.select(dev.key);
        }
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(to_utf8(dev.key).c_str());
            for (const HidCollection& c : dev.collections) {
                ImGui::BulletText("%s", collection_text(c).c_str());
            }
            if (!dev.usb) {
                ImGui::TextUnformatted(dev.not_usb_reason.c_str());
            }
            ImGui::EndTooltip();
        }
        ImGui::TableNextColumn();
        if (dev.interface_number >= 0) {
            ImGui::Text("%d", dev.interface_number);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(dev.product.c_str());
        if (!dev.collections.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s%s)", collection_text(dev.collections[0]).c_str(),
                dev.collections.size() > 1 ? ", ..." : "");
        }
        if (!dev.usb) {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void draw_details(const HidInterface& dev) {
    App& app = g_app;

    ImGui::TextUnformatted(dev.product.empty() ? "(no name)" : dev.product.c_str());
    if (!dev.manufacturer.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dev.manufacturer.c_str());
    }
    ImGui::TextDisabled("VID 0x%04X  PID 0x%04X  version 0x%04X", dev.vid, dev.pid, dev.version);
    if (dev.usb) {
        ImGui::SameLine();
        if (dev.interface_number >= 0) {
            ImGui::TextDisabled("interface %d  port %u", dev.interface_number, dev.port);
        } else {
            ImGui::TextDisabled("port %u", dev.port);
        }
    }
    ImGui::TextDisabled("%s", to_utf8(dev.key).c_str());
    std::string collections = "Top-level collections:";
    for (const HidCollection& c : dev.collections) {
        collections += "  [" + collection_text(c) + "]";
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", collections.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (!dev.usb) {
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR_WARN);
        ImGui::TextWrapped("%s", dev.not_usb_reason.c_str());
        ImGui::PopStyleColor();
        return;
    }

    const bool busy = app.helper != nullptr;
    ImGui::BeginDisabled(busy || !app.connected);
    if (ImGui::Button(app.fetched ? "Fetch again" : "Fetch")) {
        app.fetch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Fetch via WinUSB...")) {
        start_winusb_fetch();
    }
    ImGui::SetItemTooltip("Switch the interface to the WinUSB driver for a moment, ask it for the report\n"
                          "descriptor the proper way, then put the original driver back.\n"
                          "Needs administrator rights. The device stops working for a few seconds.");
    ImGui::EndDisabled();
    if (busy && !app.helper_restoring) {
        ImGui::SameLine();
        ImGui::TextColored(COLOR_WARN, "Switching to WinUSB and back...");
    } else if (!app.connected) {
        ImGui::SameLine();
        ImGui::TextDisabled("(not connected)");
    }
    if (app.descriptor.empty()) {
        if (!app.fetch_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, COLOR_ERROR);
            ImGui::TextWrapped("%s", app.fetch_error.c_str());
            ImGui::PopStyleColor();
            if (!app.via_winusb) {
                ImGui::Spacing();
                ImGui::TextWrapped("You can try \"Fetch via WinUSB...\". It needs administrator rights and the "
                                   "device stops working for a few seconds, so don't use it on the only keyboard "
                                   "or mouse you have.");
            }
        }
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button("Copy decoded")) {
        ImGui::SetClipboardText(format_decoded(app.decoded, app.descriptor).c_str());
    }
    ImGui::SetItemTooltip("Copy as C source with the decoded items as comments.");
    ImGui::SameLine();
    if (ImGui::Button("Copy hex")) {
        ImGui::SetClipboardText(format_hex(app.descriptor).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save .bin...")) {
        save_descriptor(dev, app.descriptor);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, COLOR_OK);
    ImGui::Text("%zu bytes", app.descriptor.size());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("(fetched %s%s)", app.via_winusb ? "through WinUSB at " : "at ", app.fetched_at.c_str());
    if (!app.fetch_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR_ERROR);
        ImGui::TextWrapped("%s", app.fetch_error.c_str());
        ImGui::PopStyleColor();
    }
    for (const std::string& line : app.report_lines) {
        ImGui::BulletText("%s", line.c_str());
    }
    if (!app.collections_match) {
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR_WARN);
        ImGui::TextWrapped("%s", MISMATCH_WARNING);
        ImGui::PopStyleColor();
    }
    for (const std::string& p : app.decoded.problems) {
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR_WARN);
        ImGui::TextWrapped("%s", p.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (!ImGui::BeginTabBar("views")) {
        return;
    }
    if (ImGui::BeginTabItem("Decoded")) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
        ImGui::PushFont(app.mono_font, 0.0f);
        if (ImGui::BeginTable("decoded", 3, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Offset");
            ImGui::TableSetupColumn("Bytes", 0, ImGui::CalcTextSize("00 00 00 00 00  ").x);
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            const float indent_w = ImGui::CalcTextSize("  ").x;
            ImGuiListClipper clipper;
            clipper.Begin((int) app.decoded.items.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const DecodedItem& item = app.decoded.items[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%04zX", item.offset);
                    ImGui::TableNextColumn();
                    std::string bytes;
                    for (size_t k = 0; k < item.size; k++) {
                        char b[4];
                        snprintf(b, sizeof(b), k ? " %02X" : "%02X", app.descriptor[item.offset + k]);
                        bytes += b;
                    }
                    ImGui::TextUnformatted(bytes.c_str());
                    ImGui::TableNextColumn();
                    ImVec4 color = item.kind == ItemKind::MAIN     ? COLOR_MAIN
                                   : item.kind == ItemKind::GLOBAL ? COLOR_GLOBAL
                                   : item.kind == ItemKind::LOCAL  ? COLOR_LOCAL
                                   : item.kind == ItemKind::BROKEN ? COLOR_ERROR
                                                                   : COLOR_DIM;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + item.indent * indent_w);
                    ImGui::TextColored(color, "%s", item.text.c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopFont();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Hex")) {
        ImGui::PushFont(app.mono_font, 0.0f);
        if (ImGui::BeginChild("hex", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGuiListClipper clipper;
            clipper.Begin((int) (app.descriptor.size() + 15) / 16);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                    char line[128];
                    int n = snprintf(line, sizeof(line), "%04X  ", row * 16);
                    for (size_t k = row * 16; k < std::min<size_t>(row * 16 + 16, app.descriptor.size()); k++) {
                        n += snprintf(line + n, sizeof(line) - n, "%02X ", app.descriptor[k]);
                    }
                    ImGui::TextUnformatted(line);
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

static void draw_ui() {
    App& app = g_app;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (app.restore_needed) {
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR_WARN);
        ImGui::TextWrapped("%s is still on the WinUSB driver from an earlier attempt.",
            to_utf8(app.restore_id).c_str());
        ImGui::PopStyleColor();
        ImGui::BeginDisabled(app.helper != nullptr);
        if (ImGui::Button("Restore driver")) {
            start_restore();
        }
        ImGui::EndDisabled();
        if (!app.helper_error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(COLOR_ERROR, "%s", app.helper_error.c_str());
        }
        ImGui::Separator();
    }

    const float left_w = std::max(em * 22, ImGui::GetContentRegionAvail().x * 0.38f);
    if (ImGui::BeginChild("left", ImVec2(left_w, 0))) {
        draw_device_list();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("right", ImVec2(0, 0))) {
        if (!app.selected_key.empty()) {
            draw_details(app.selected_dev);
        } else {
            ImGui::TextDisabled("Select a device on the left to fetch its HID report descriptor.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------

static LRESULT WINAPI main_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            g_resize_width = (UINT) LOWORD(lparam);
            g_resize_height = (UINT) HIWORD(lparam);
            return 0;
        case WM_GETMINMAXINFO: {
            float s = GetDpiForWindow(hwnd) / 96.0f;
            auto* mmi = (MINMAXINFO*) lparam;
            mmi->ptMinTrackSize.x = (LONG) (800 * s);
            mmi->ptMinTrackSize.y = (LONG) (480 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_pending_dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT* r = (const RECT*) lparam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DEVICECHANGE:
            if (wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) {
                // Devices come and go in bursts (one per collection), so wait a bit.
                g_app.refresh_at = GetTickCount64() + 300;
            }
            return TRUE;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {  // disable ALT application menu
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && wcsncmp(argv[1], L"--winusb-", 9) == 0) {
        return run_helper(argc, argv);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    App& app = g_app;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ReportDescriptorViewer";
    RegisterClassExW(&wc);

    const float primary_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Report Descriptor Viewer", WS_OVERLAPPEDWINDOW,
        (int) (80 * primary_scale), (int) (80 * primary_scale), (int) (1240 * primary_scale),
        (int) (820 * primary_scale), nullptr, nullptr, instance, nullptr);
    app.main_hwnd = hwnd;
    const float scale = GetDpiForWindow(hwnd) / 96.0f;

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"Report Descriptor Viewer", MB_ICONERROR);
        return 1;
    }

    // Get told when HID devices are plugged in or removed.
    DEV_BROADCAST_DEVICEINTERFACE_W filter = { sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.dbcc_classguid);
    HDEVNOTIFY notify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    app.refresh();

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    app.mono_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 14.0f);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    bool done = false;
    while (!done) {
        const bool minimized = IsIconic(hwnd) != 0;
        if (minimized || g_swap_chain_occluded) {
            // Nothing to draw: sleep until something happens.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        if (app.refresh_at != 0 && GetTickCount64() >= app.refresh_at) {
            app.refresh_at = 0;
            app.refresh();
        }
        if (app.helper != nullptr && WaitForSingleObject(app.helper, 0) == WAIT_OBJECT_0) {
            finish_helper();
        }

        if (minimized) {
            continue;
        }
        if (g_swap_chain_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            continue;
        }
        g_swap_chain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }
        if (g_pending_dpi_scale > 0) {
            apply_style(g_pending_dpi_scale);
            g_pending_dpi_scale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_device_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target_view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap_chain->Present(1, 0);
        g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    if (notify != nullptr) {
        UnregisterDeviceNotification(notify);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
