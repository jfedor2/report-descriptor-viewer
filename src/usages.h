#pragma once

#include <cstdint>
#include <string>

// Name of a usage page, like "Generic Desktop" or "Vendor Defined 0xFF00".
std::string usage_page_name(uint16_t page);

// Name of a usage on the given page, or an empty string if we don't know it.
std::string usage_name(uint16_t page, uint16_t usage);
