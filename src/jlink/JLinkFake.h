#pragma once
// Test hooks of the in-process fake DLL (JLinkFakeDLL.cpp); only linked with JLINK_USE_FAKE_DLL.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Every JLINK_WriteU32 since the last clear, as (address, value), in order.
std::vector<std::pair<std::uint32_t,
                      std::uint32_t>>
fakeJLinkWrites();
void fakeJLinkClearWrites();

// Every call that moves the target since the last clear, in order: "reset", "halt", "go",
// "download <file>", "w4 <address> <value>", "wreg <index> <value>" (numbers as 0x%08x, the
// register index decimal), and the end of RTT and of the session: "rtt stop", "close".
std::vector<std::string> fakeJLinkCalls();
void                     fakeJLinkClearCalls();
