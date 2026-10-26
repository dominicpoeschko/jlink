// JLink's pre-reset writes and the start of a RAM image, against the fake DLL (JLINK_USE_FAKE_DLL). Runs in its
// build directory: it writes a small hex file there.

#include "jlink/JLink.hpp"

#include "Check.hpp"
#include "jlink/JLinkFake.h"

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
bool throws(std::string_view line) {
    try {
        JLink::parseCommand(line);
    } catch(std::runtime_error const&) { return true; }
    return false;
}
}   // namespace

int main() {
    CHECK((JLink::parseCommand("w4 0x4001A004 0x01000000")
           == JLink::MemoryWrite{0x4001A004, 0x01000000}),
          "w4 in hex");
    CHECK((JLink::parseCommand("  W4\t16  0X10 ") == JLink::MemoryWrite{16, 16}),
          "decimal, upper case, extra blanks");
    CHECK(throws("mem32 0x0 1"), "another command is refused");
    CHECK(throws("w4 0x0"), "a missing value is refused");
    CHECK(throws("w4 0x0 1 2"), "an extra word is refused");
    CHECK(throws("w4 0x100000000 1"), "more than 32 bits is refused");
    CHECK(throws("w4 0xZZ 1"), "not a number is refused");
    CHECK(throws("w4 0x 1"), "an empty number is refused");

    {
        JLink jlink{"fake",
                    4000,
                    JLink::Connection{},
                    [](std::string_view) {},
                    [](std::string_view) {}};
        fakeJLinkClearWrites();
        jlink.resetTarget();
        CHECK(fakeJLinkWrites().empty(), "no commands, no writes");

        jlink.setPreResetCommands({
          JLink::MemoryWrite{0x4001A004, 0x01000000},
          JLink::MemoryWrite{0x4001B004, 0x01000000}
        });
        jlink.resetTarget();
        auto const writes = fakeJLinkWrites();
        CHECK(writes.size() == 2 && writes[0].first == 0x4001A004 && writes[1].first == 0x4001B004
                && writes[1].second == 0x01000000,
              "resetTarget writes the commands in order");

        // the reset line: the commands, then exactly one pulse of the pin and no JLINK_Reset (whose type-2 strategy
        // pulses again when its vector catch is lost)
        fakeJLinkClearWrites();
        fakeJLinkClearCalls();
        jlink.pulseResetPin(std::chrono::milliseconds{1});
        CHECK((fakeJLinkCalls()
               == std::vector<std::string>{"w4 0x4001a004 0x01000000",
                                           "w4 0x4001b004 0x01000000",
                                           "reset pin low",
                                           "reset pin high"}),
              "pulseResetPin: the commands, then one pulse, no reset strategy");
    }

    // A RAM image: reset and halt, download, its vector table into VTOR / MSP / xPSR / PC, go -
    // and no reset after the download (it would boot flash). The SDK's J-Link script for a
    // RAM_ONLY image does the same (cmake/jlink.cmake).
    {
        std::string const hexFile{"jlink_test_ram_image.hex"};
        std::ofstream{hexFile} << ":020000042000DA\n"
                                  ":080000000020082099010020F6\n"
                                  ":00000001FF\n";
        JLink jlink{"fake",
                    4000,
                    JLink::Connection{},
                    [](std::string_view) {},
                    [](std::string_view) {}};
        jlink.setPreResetCommands({
          JLink::MemoryWrite{0x4001A004, 0x01000000}
        });
        fakeJLinkClearCalls();
        jlink.flashRamImage(hexFile, JLink::RamImageStart{0x2000'0000, 0x2008'2000, 0x2000'0199});
        std::vector<std::string> expected{"w4 0x4001a004 0x01000000",
                                          "reset",
                                          "halt",
                                          "w4 0x4001a004 0x01000000",
                                          "download " + hexFile};
        // every NVIC enable and pending bit cleared, as a reset would: the fake's CPUID reads 0,
        // not Armv6-M, so all sixteen NVIC_ICERn / NVIC_ICPRn
        for(unsigned n = 0; n != 16; ++n) {
            expected.push_back(std::format("w4 {:#010x} 0xffffffff", 0xE000E180U + 4U * n));
            expected.push_back(std::format("w4 {:#010x} 0xffffffff", 0xE000E280U + 4U * n));
        }
        for(std::string const& line : {std::string{"w4 0xe000ed08 0x20000000"},   // VTOR
                                       std::string{"wreg 17 0x20082000"},         // MSP
                                       std::string{"wreg 16 0x01000000"},         // xPSR: Thumb
                                       std::string{"wreg 15 0x20000198"},   // PC without bit 0
                                       std::string{"go"}})
        {
            expected.push_back(line);
        }
        auto const calls = fakeJLinkCalls();
        CHECK(calls == expected, "the RAM image start sequence");
        if(calls != expected) {
            for(auto const& c : calls) { std::printf("  %s\n", c.c_str()); }
        }

        // a download that did not land (the target holds other words): no jump into it
        fakeJLinkClearCalls();
        bool threw = false;
        try {
            jlink.flashRamImage(hexFile,
                                JLink::RamImageStart{0x2000'0000, 0x2008'1000, 0x2000'0199});
        } catch(std::runtime_error const& e) {
            threw = std::string_view{e.what()}.contains("after the download");
        }
        CHECK(threw, "words the target does not hold are an error");
        auto const after = fakeJLinkCalls();
        CHECK(!after.empty() && after.back() == "download " + hexFile,
              "and nothing is written or started after the download");
        jlink.go();
    }

    // The end of a session for an announced reset: RTT stops, the ack is written, the session
    // closes - and nothing after the ack touches the target (an RP2040 resetting itself would be
    // "recovered" through its Rescue DP by any access).
    {
        std::vector<std::string> calls;
        {
            JLink jlink{"fake",
                        4000,
                        JLink::Connection{},
                        [](std::string_view) {},
                        [](std::string_view) {}};
            (void)jlink.startRtt(4, 0);
            fakeJLinkClearCalls();
            jlink.stopRtt();
            jlink.writeWord(0x2000'1000, 7);
        }
        calls = fakeJLinkCalls();
        std::vector<std::string> const expected{"rtt stop", "w4 0x20001000 0x00000007", "close"};
        CHECK(calls == expected, "stopRtt, the ack, then only the close");
        if(calls != expected) {
            for(auto const& c : calls) { std::printf("  %s\n", c.c_str()); }
        }
    }

    return jlink_test::report();
}
