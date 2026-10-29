// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Report descriptor parser, following "Device Class Definition for HID 1.11",
// section 6.2.2.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>

#include "decoder.h"
#include "usages.h"

namespace {

struct GlobalState {
    uint16_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint32_t report_id = 0;
};

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list args;
    va_start(args, f);
    vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return buf;
}

int32_t as_signed(uint32_t v, int size) {
    switch (size) {
        case 1:
            return (int8_t) v;
        case 2:
            return (int16_t) v;
        default:
            return (int32_t) v;
    }
}

// Usage with the current usage page, or an extended (page:usage) one.
std::string usage_text(uint32_t v, int size, uint16_t page) {
    if (size == 4) {
        const uint16_t p = (uint16_t) (v >> 16);
        const uint16_t u = (uint16_t) v;
        std::string name = usage_name(p, u);
        return usage_page_name(p) + ": " + (name.empty() ? fmt("0x%02X", u) : name);
    }
    std::string name = usage_name(page, (uint16_t) v);
    return name.empty() ? fmt("0x%02X", v) : name;
}

std::string main_flags(uint32_t v, bool input) {
    std::string s;
    s += (v & 0x01) ? "Const" : "Data";
    s += (v & 0x02) ? ",Var" : ",Array";
    s += (v & 0x04) ? ",Rel" : ",Abs";
    // The rest only when they're not the default.
    if (v & 0x08) {
        s += ",Wrap";
    }
    if (v & 0x10) {
        s += ",Non Linear";
    }
    if (v & 0x20) {
        s += ",No Preferred State";
    }
    if (v & 0x40) {
        s += ",Null State";
    }
    if (!input && (v & 0x80)) {
        s += ",Volatile";
    }
    if (v & 0x100) {
        s += ",Buffered Bytes";
    }
    return s;
}

std::string collection_type(uint32_t v) {
    static const char* names[] = { "Physical", "Application", "Logical", "Report", "Named Array", "Usage Switch",
        "Usage Modifier" };
    if (v < sizeof(names) / sizeof(names[0])) {
        return names[v];
    }
    if (v >= 0x80 && v <= 0xFF) {
        return fmt("Vendor Defined 0x%02X", v);
    }
    return fmt("Reserved 0x%02X", v);
}

std::string unit_text(uint32_t v) {
    if (v == 0) {
        return "None";
    }
    static const char* systems[] = { "None", "SI Linear", "SI Rotation", "English Linear", "English Rotation" };
    static const char* units[6][5] = {
        { "", "cm", "rad", "in", "deg" },      // length
        { "", "g", "g", "slug", "slug" },      // mass
        { "", "s", "s", "s", "s" },            // time
        { "", "K", "K", "F", "F" },            // temperature
        { "", "A", "A", "A", "A" },            // current
        { "", "cd", "cd", "cd", "cd" },        // luminous intensity
    };
    const uint32_t system = v & 0xF;
    if (system > 4) {
        return fmt("Vendor/Reserved 0x%08X", v);
    }
    std::string s = systems[system];
    std::string parts;
    for (int i = 0; i < 6; i++) {
        int exp = (v >> (4 * (i + 1))) & 0xF;
        if (exp == 0) {
            continue;
        }
        if (exp >= 8) {
            exp -= 16;
        }
        if (!parts.empty()) {
            parts += "*";
        }
        parts += system == 0 ? "?" : units[i][system];
        if (exp != 1) {
            parts += fmt("^%d", exp);
        }
    }
    if (v >> 28) {
        parts += fmt(" (0x%08X)", v);
    }
    return parts.empty() ? s : s + ": " + parts;
}

}  // namespace

DecodedDescriptor decode_report_descriptor(const std::vector<uint8_t>& data) {
    DecodedDescriptor out;
    GlobalState global;
    std::vector<GlobalState> stack;
    std::map<uint32_t, ReportInfo> reports;
    int indent = 0;
    // The first usage since the last main item, for naming collections.
    bool have_usage = false;
    uint16_t usage_page = 0;
    uint16_t usage = 0;

    size_t pos = 0;
    while (pos < data.size()) {
        DecodedItem item;
        item.offset = pos;
        const uint8_t prefix = data[pos];

        if (prefix == 0xFE) {  // long item
            if (pos + 3 > data.size() || pos + 3 + data[pos + 1] > data.size()) {
                item.size = data.size() - pos;
                item.kind = ItemKind::BROKEN;
                item.text = "Truncated long item";
                item.indent = indent;
                out.items.push_back(item);
                out.problems.push_back(fmt("Truncated long item at offset %zu.", pos));
                break;
            }
            item.size = 3 + data[pos + 1];
            item.kind = ItemKind::OTHER;
            item.text = fmt("Long Item (tag 0x%02X, %d data bytes)", data[pos + 2], data[pos + 1]);
            item.indent = indent;
            out.items.push_back(item);
            pos += item.size;
            continue;
        }

        const int size = (prefix & 3) == 3 ? 4 : (prefix & 3);
        const int type = (prefix >> 2) & 3;
        const int tag = prefix >> 4;
        item.size = 1 + size;
        if (pos + item.size > data.size()) {
            item.size = data.size() - pos;
            item.kind = ItemKind::BROKEN;
            item.text = "Truncated item";
            item.indent = indent;
            out.items.push_back(item);
            out.problems.push_back(fmt("Truncated item at offset %zu.", pos));
            break;
        }
        uint32_t v = 0;
        for (int i = 0; i < size; i++) {
            v |= (uint32_t) data[pos + 1 + i] << (8 * i);
        }
        const int32_t sv = as_signed(v, size);

        std::string& t = item.text;
        switch (type) {
            case 0:  // main
                item.kind = ItemKind::MAIN;
                switch (tag) {
                    case 0x8:
                    case 0x9:
                    case 0xB: {
                        const ReportType rt = tag == 0x8 ? REPORT_INPUT : tag == 0x9 ? REPORT_OUTPUT : REPORT_FEATURE;
                        static const char* names[] = { "Input", "Output", "Feature" };
                        t = fmt("%s (%s)", names[rt], main_flags(v, rt == REPORT_INPUT).c_str());
                        ReportInfo& r = reports[global.report_id];
                        r.id = global.report_id;
                        r.bits[rt] += global.report_size * global.report_count;
                        break;
                    }
                    case 0xA:
                        t = "Collection (" + collection_type(v) + ")";
                        if (indent == 0) {
                            out.top_level.emplace_back(have_usage ? usage_page : 0, have_usage ? usage : 0);
                        }
                        break;
                    case 0xC:
                        if (indent == 0) {
                            out.problems.push_back(fmt("End Collection without Collection at offset %zu.", pos));
                        } else {
                            indent--;
                        }
                        t = "End Collection";
                        break;
                    default:
                        item.kind = ItemKind::OTHER;
                        t = fmt("Reserved main item (tag 0x%X)", tag);
                        break;
                }
                break;
            case 1:  // global
                item.kind = ItemKind::GLOBAL;
                switch (tag) {
                    case 0x0:
                        global.usage_page = (uint16_t) v;
                        t = "Usage Page (" + usage_page_name((uint16_t) v) + ")";
                        break;
                    case 0x1:
                        t = fmt("Logical Minimum (%d)", sv);
                        break;
                    case 0x2:
                        t = fmt("Logical Maximum (%d)", sv);
                        break;
                    case 0x3:
                        t = fmt("Physical Minimum (%d)", sv);
                        break;
                    case 0x4:
                        t = fmt("Physical Maximum (%d)", sv);
                        break;
                    case 0x5:
                        // Usually a 4-bit signed nibble, but some write it as a plain signed number.
                        t = fmt("Unit Exponent (%d)", (size == 1 && v <= 0xF) ? (v >= 8 ? (int) v - 16 : (int) v) : sv);
                        break;
                    case 0x6:
                        t = "Unit (" + unit_text(v) + ")";
                        break;
                    case 0x7:
                        global.report_size = v;
                        t = fmt("Report Size (%u)", v);
                        break;
                    case 0x8:
                        global.report_id = v;
                        out.uses_report_ids = true;
                        if (v == 0) {
                            out.problems.push_back(fmt("Report ID 0 at offset %zu is not allowed.", pos));
                        }
                        t = fmt("Report ID (%u)", v);
                        break;
                    case 0x9:
                        global.report_count = v;
                        t = fmt("Report Count (%u)", v);
                        break;
                    case 0xA:
                        stack.push_back(global);
                        t = "Push";
                        break;
                    case 0xB:
                        if (stack.empty()) {
                            out.problems.push_back(fmt("Pop without Push at offset %zu.", pos));
                        } else {
                            global = stack.back();
                            stack.pop_back();
                        }
                        t = "Pop";
                        break;
                    default:
                        item.kind = ItemKind::OTHER;
                        t = fmt("Reserved global item (tag 0x%X)", tag);
                        break;
                }
                break;
            case 2:  // local
                item.kind = ItemKind::LOCAL;
                switch (tag) {
                    case 0x0:
                        t = "Usage (" + usage_text(v, size, global.usage_page) + ")";
                        if (!have_usage) {
                            have_usage = true;
                            usage_page = size == 4 ? (uint16_t) (v >> 16) : global.usage_page;
                            usage = (uint16_t) v;
                        }
                        break;
                    case 0x1:
                        t = "Usage Minimum (" + usage_text(v, size, global.usage_page) + ")";
                        break;
                    case 0x2:
                        t = "Usage Maximum (" + usage_text(v, size, global.usage_page) + ")";
                        break;
                    case 0x3:
                        t = fmt("Designator Index (%u)", v);
                        break;
                    case 0x4:
                        t = fmt("Designator Minimum (%u)", v);
                        break;
                    case 0x5:
                        t = fmt("Designator Maximum (%u)", v);
                        break;
                    case 0x7:
                        t = fmt("String Index (%u)", v);
                        break;
                    case 0x8:
                        t = fmt("String Minimum (%u)", v);
                        break;
                    case 0x9:
                        t = fmt("String Maximum (%u)", v);
                        break;
                    case 0xA:
                        t = v == 1 ? "Delimiter (Open)" : v == 0 ? "Delimiter (Close)" : fmt("Delimiter (%u)", v);
                        break;
                    default:
                        item.kind = ItemKind::OTHER;
                        t = fmt("Reserved local item (tag 0x%X)", tag);
                        break;
                }
                break;
            default:
                item.kind = ItemKind::OTHER;
                t = fmt("Reserved item type (tag 0x%X)", tag);
                break;
        }

        item.indent = indent;
        if (type == 0) {
            have_usage = false;
            if (tag == 0xA) {
                indent++;
            }
        }
        out.items.push_back(item);
        pos += item.size;
    }

    if (indent > 0) {
        out.problems.push_back(fmt("%d collection(s) not closed.", indent));
    }
    for (auto& [id, r] : reports) {
        if (r.bits[REPORT_INPUT] || r.bits[REPORT_OUTPUT] || r.bits[REPORT_FEATURE]) {
            out.reports.push_back(r);
        }
    }
    return out;
}

bool same_top_level(const DecodedDescriptor& decoded, std::vector<std::pair<uint16_t, uint16_t>> expected) {
    std::vector<std::pair<uint16_t, uint16_t>> actual = decoded.top_level;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

std::string format_decoded(const DecodedDescriptor& decoded, const std::vector<uint8_t>& data) {
    std::string out;
    for (const DecodedItem& item : decoded.items) {
        std::string bytes;
        for (size_t i = 0; i < item.size; i++) {
            bytes += fmt("0x%02X, ", data[item.offset + i]);
        }
        bytes.pop_back();
        if (bytes.size() < 30) {
            bytes.resize(30, ' ');
        }
        out += bytes + " // " + std::string(item.indent * 2, ' ') + item.text + "\n";
    }
    out += fmt("\n// %zu bytes\n", data.size());
    return out;
}

std::string format_hex(const std::vector<uint8_t>& data) {
    std::string out;
    for (size_t i = 0; i < data.size(); i++) {
        out += fmt("%02X", data[i]);
        out += (i % 16 == 15 || i + 1 == data.size()) ? "\n" : " ";
    }
    return out;
}

std::vector<std::string> format_reports(const DecodedDescriptor& decoded) {
    static const char* names[] = { "Input", "Output", "Feature" };
    std::vector<std::string> lines;
    for (const ReportInfo& r : decoded.reports) {
        std::string line = decoded.uses_report_ids ? fmt("Report ID %u:", r.id) : std::string("No report ID:");
        bool first = true;
        for (int t = 0; t < REPORT_TYPE_COUNT; t++) {
            if (r.bits[t] == 0) {
                continue;
            }
            line += first ? " " : ", ";
            first = false;
            const uint32_t bytes = (r.bits[t] + 7) / 8;
            line += fmt("%s %u byte%s", names[t], bytes, bytes == 1 ? "" : "s");
            if (r.bits[t] % 8) {
                line += fmt(" (%u bits)", r.bits[t]);
            }
        }
        lines.push_back(line);
    }
    return lines;
}
