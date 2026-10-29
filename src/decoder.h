#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class ItemKind {
    MAIN,
    GLOBAL,
    LOCAL,
    OTHER,  // long and reserved items
    BROKEN,
};

struct DecodedItem {
    size_t offset = 0;
    size_t size = 0;  // including the prefix byte(s)
    int indent = 0;
    ItemKind kind = ItemKind::OTHER;
    std::string text;
};

enum ReportType {
    REPORT_INPUT,
    REPORT_OUTPUT,
    REPORT_FEATURE,
    REPORT_TYPE_COUNT,
};

struct ReportInfo {
    uint32_t id = 0;
    uint32_t bits[REPORT_TYPE_COUNT] = {};
};

struct DecodedDescriptor {
    std::vector<DecodedItem> items;
    std::vector<ReportInfo> reports;  // sorted by ID
    bool uses_report_ids = false;
    std::vector<std::pair<uint16_t, uint16_t>> top_level;  // usage page and usage of top-level collections
    std::vector<std::string> problems;
};

DecodedDescriptor decode_report_descriptor(const std::vector<uint8_t>& data);

// Whether the top-level collections are the same as the expected ones, in any order.
bool same_top_level(const DecodedDescriptor& decoded, std::vector<std::pair<uint16_t, uint16_t>> expected);

// "0x05, 0x01,        // Usage Page (Generic Desktop)" lines, usable as C source.
std::string format_decoded(const DecodedDescriptor& decoded, const std::vector<uint8_t>& data);

// Plain hex dump, 16 bytes per line.
std::string format_hex(const std::vector<uint8_t>& data);

// One line per report, like "Report ID 1: Input 8 bytes, Output 1 byte".
std::vector<std::string> format_reports(const DecodedDescriptor& decoded);
