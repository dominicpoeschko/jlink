#pragma once
// Test hooks of the in-process fake DLL (JLinkFakeDLL.cpp); only linked with JLINK_USE_FAKE_DLL.

#include <cstdint>
#include <utility>
#include <vector>

// Every JLINK_WriteU32 since the last clear, as (address, value), in order.
std::vector<std::pair<std::uint32_t,
                      std::uint32_t>>
fakeJLinkWrites();
void fakeJLinkClearWrites();
