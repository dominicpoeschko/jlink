// in-process fake of the SEGGER jlink DLL for testing without hardware
//
// Simulates a target with named RTT buffers: log up buffers (silent) plus duplex up/down
// pairs whose "target" echoes every received byte back, driven by a background thread so
// the data flow crosses threads like the real DLL.
//
// Configuration via environment variables:
//   FAKE_JLINK_LOG_BUFFERS      number of (silent) log up buffers, default 2
//   FAKE_JLINK_DUPLEX_CHANNELS  comma separated channel names, default "echo"
//   FAKE_JLINK_NO_GETDESC       if set, getDesc fails so hosts exercise their positional
//                               pairing fallback
//
// The core is halted by JLINK_Halt and runs again on JLINK_Go. JLINK_DownloadFile reads an Intel
// HEX file, if there is one by that name, into the fake memory, which JLINK_ReadMem then returns;
// every other byte reads as the low byte of its address.

#include "JLinkFake.h"

#include "JLinkDLL.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t FakeBufferCapacity = 4096;

struct FakeChannel {
    std::string       name;
    std::vector<char> upFifo;     //"target" -> host
    std::vector<char> downFifo;   // host -> "target"
};

struct FakeJLink {
    std::mutex mutex;

    void (*logCallback)(char const*)   = nullptr;
    void (*errorCallback)(char const*) = nullptr;

    bool open{false};
    bool rttRunning{false};

    std::size_t              numLogBuffers{2};
    std::vector<FakeChannel> duplexChannels;
    bool                     noGetDesc{false};

    std::uint32_t bytesReadByHost{0};

    std::jthread targetThread;

    FakeJLink() {
        if(char const* env = std::getenv("FAKE_JLINK_LOG_BUFFERS")) {
            numLogBuffers = static_cast<std::size_t>(std::strtoul(env, nullptr, 10));
        }
        std::string names{"echo"};
        if(char const* env = std::getenv("FAKE_JLINK_DUPLEX_CHANNELS")) { names = env; }
        for(std::size_t pos{}; pos <= names.size();) {
            auto const comma = std::min(names.find(',', pos), names.size());
            if(comma != pos) { duplexChannels.push_back({names.substr(pos, comma - pos), {}, {}}); }
            pos = comma + 1;
        }
        noGetDesc = std::getenv("FAKE_JLINK_NO_GETDESC") != nullptr;

        targetThread = std::jthread{[this](std::stop_token const& stoken) { targetLoop(stoken); }};
    }

    // the simulated target: echo every byte arriving on a down buffer back to the paired
    // up buffer, respecting the up buffer capacity like a trim mode write would
    void targetLoop(std::stop_token const& stoken) {
        while(!stoken.stop_requested()) {
            {
                std::lock_guard<std::mutex> const lock{mutex};
                if(rttRunning) {
                    for(auto& channel : duplexChannels) {
                        auto const space = FakeBufferCapacity - channel.upFifo.size();
                        auto const n     = std::min(space, channel.downFifo.size());
                        if(n != 0) {
                            channel.upFifo.insert(
                              channel.upFifo.end(),
                              channel.downFifo.begin(),
                              std::next(channel.downFifo.begin(), static_cast<std::ptrdiff_t>(n)));
                            channel.downFifo.erase(
                              channel.downFifo.begin(),
                              std::next(channel.downFifo.begin(), static_cast<std::ptrdiff_t>(n)));
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    void log(char const* msg) {
        if(logCallback != nullptr) { logCallback(msg); }
    }

    std::size_t numUpBuffers() const { return numLogBuffers + duplexChannels.size(); }
};

FakeJLink& fake() {
    static FakeJLink instance;
    return instance;
}

}   // namespace

namespace {
constexpr std::string_view fakeProduct{"J-Link fake"};
constexpr std::string_view fakeNick{"fake"};
constexpr std::string_view fakeFirmware{"J-Link fake compiled today"};
constexpr std::uint32_t    fakeSerial{1};
constexpr int              fakeHwVersion{10000};   // 1.00

template<typename Size>
void copyCString(std::string_view text,
                 char*            buffer,
                 Size             bufferSize) {
    if(buffer == nullptr || bufferSize <= 0) { return; }
    auto const n = std::min(text.size(), static_cast<std::size_t>(bufferSize) - 1);
    std::copy_n(text.begin(), n, buffer);
    buffer[n] = '\0';
}
}   // namespace

namespace {
std::mutex                                           writesMutex;
std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
std::vector<std::string>                             calls;           // guarded by writesMutex
std::map<std::uint32_t, unsigned char>               memory;          // guarded by writesMutex
bool                                                 halted{false};   // guarded by writesMutex

void recordCall(std::string call) {
    std::lock_guard<std::mutex> const lock{writesMutex};
    calls.push_back(std::move(call));
}

// Data (00), extended segment (02) and extended linear (04) address records; no checksum check.
void loadHex(char const* fileName) {
    std::ifstream in{fileName};
    std::string   line;
    std::uint32_t base = 0;
    auto const    byteAt
      = [&](std::size_t i) { return std::stoul(line.substr(1 + 2 * i, 2), nullptr, 16); };
    while(std::getline(in, line)) {
        while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
        if(line.size() < 11 || line.front() != ':') { continue; }
        auto const count  = byteAt(0);
        auto const offset = static_cast<std::uint32_t>((byteAt(1) << 8U) | byteAt(2));
        auto const type   = byteAt(3);
        if(line.size() < 11 + 2 * count) { continue; }
        if(type == 1) { break; }
        if(type == 2 && count == 2) {
            base = static_cast<std::uint32_t>(((byteAt(4) << 8U) | byteAt(5)) << 4U);
        } else if(type == 4 && count == 2) {
            base = static_cast<std::uint32_t>(((byteAt(4) << 8U) | byteAt(5)) << 16U);
        } else if(type == 0) {
            std::lock_guard<std::mutex> const lock{writesMutex};
            for(std::size_t i = 0; i != count; ++i) {
                memory[base + offset + static_cast<std::uint32_t>(i)]
                  = static_cast<unsigned char>(byteAt(4 + i));
            }
        }
    }
}
}   // namespace

extern "C" {
int JLINK_EMU_GetNumDevices() { return 1; }

// One probe, on USB only: a name that is not on USB makes the host look on the network
// and find nothing there.
int JLINK_EMU_GetList(int             hostInterfaces,
                      EmuConnectInfo* infos,
                      int             maxInfos) {
    if((hostInterfaces & EmuHostUsb) == 0) { return 0; }
    if(maxInfos < 1 || infos == nullptr) { return 1; }
    infos[0]              = EmuConnectInfo{};
    infos[0].serialNumber = fakeSerial;
    infos[0].connection   = EmuHostUsb;
    infos[0].hwVersion    = static_cast<std::uint32_t>(fakeHwVersion);
    copyCString(fakeProduct, infos[0].product.data(), infos[0].product.size());
    copyCString(fakeNick, infos[0].nickName.data(), infos[0].nickName.size());
    copyCString(fakeFirmware, infos[0].fwString.data(), infos[0].fwString.size());
    return 1;
}

int JLINK_EMU_SelectByUSBSN(std::uint32_t serialNumber) {
    return serialNumber == fakeSerial ? 0 : -1;
}

void JLINK_EMU_SelectIPBySN(std::uint32_t) {}

std::uint32_t JLINK_GetSN() { return fakeSerial; }

void JLINK_EMU_GetProductName(char*         buffer,
                              std::uint32_t bufferSize) {
    copyCString(fakeProduct, buffer, bufferSize);
}

int JLINK_GetHardwareVersion() { return fakeHwVersion; }

void JLINK_GetFirmwareString(char* buffer,
                             int   bufferSize) {
    copyCString(fakeFirmware, buffer, bufferSize);
}

char const* JLINK_OpenEx(void (*log)(char const*),
                         void (*errorLog)(char const*)) {
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    f.logCallback   = log;
    f.errorCallback = errorLog;
    f.open          = true;
    f.log("fake jlink dll: open");
    return nullptr;
}

char JLINK_IsOpen() {
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    return f.open ? 1 : 0;
}

int JLINK_TIF_Select(int) { return 0; }

void JLINK_SetSpeed(std::uint32_t) {}

char JLINK_IsConnected() {
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    return f.open ? 1 : 0;
}

int JLINK_Connect() { return 0; }

char JLINK_IsHalted() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    return halted ? 1 : 0;
}

// Each byte reads as the low byte of its address, so a test can check what it got; DHCSR reads
// as a running core that was not reset (S_RESET_ST, bit 25, clear).
int JLINK_ReadMem(std::uint32_t address,
                  std::uint32_t numBytes,
                  void*         data) {
    auto* const out = static_cast<unsigned char*>(data);
    if(address == 0xE000'EDF0 && numBytes == 4) {
        std::uint32_t const dhcsr = 0x0100'0000U;
        for(std::uint32_t i = 0; i != numBytes; ++i) {
            out[i] = static_cast<unsigned char>(dhcsr >> (8 * i));
        }
        return 0;
    }
    std::lock_guard<std::mutex> const lock{writesMutex};
    for(std::uint32_t i = 0; i != numBytes; ++i) {
        auto const loaded = memory.find(address + i);
        out[i] = loaded != memory.end() ? loaded->second
                                        : static_cast<unsigned char>((address + i) & 0xFFU);
    }
    return 0;
}

int JLINK_WriteU32(std::uint32_t address,
                   std::uint32_t data) {
    std::lock_guard<std::mutex> const lock{writesMutex};
    writes.emplace_back(address, data);
    calls.push_back(std::format("w4 {:#010x} {:#010x}", address, data));
    return 0;
}

char JLINK_WriteReg(int           registerIndex,
                    std::uint32_t data) {
    std::lock_guard<std::mutex> const lock{writesMutex};
    if(!halted) { return -1; }   // the real DLL cannot write a running core's registers
    calls.push_back(std::format("wreg {} {:#010x}", registerIndex, data));
    return 0;
}

// A register reads as 0x1000 + its index.
std::uint32_t JLINK_ReadReg(int registerIndex) {
    return 0x1000U + static_cast<std::uint32_t>(registerIndex);
}

void JLINK_Halt() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    halted = true;
    calls.emplace_back("halt");
}

void JLINK_Go() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    halted = false;
    calls.emplace_back("go");
}

int JLINK_ClrBPEx(unsigned) { return 0; }

int JLINK_ExecCommand(char const*,
                      char*,
                      int) {
    return 0;
}

int JLINK_HasError() { return 0; }

void JLINK_Close() {
    recordCall("close");
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    f.open       = false;
    f.rttRunning = false;
}

char JLINK_SelectUSB(int) { return 0; }

char JLINK_SelectIP(char const*,
                    int) {
    return 0;
}

int JLINK_Reset() {
    recordCall("reset");
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    for(auto& channel : f.duplexChannels) {
        channel.upFifo.clear();
        channel.downFifo.clear();
    }
    return 0;
}

int JLINK_SetResetType(std::uint8_t) { return 0; }

int JLINK_DownloadFile(char const* sFileName,
                       std::uint32_t) {
    recordCall(std::string{"download "} + sFileName);
    loadHex(sFileName);
    auto& f = fake();
    {
        std::lock_guard<std::mutex> const lock{f.mutex};
        f.log((std::string{"fake jlink dll: flash "} + sFileName).c_str());
    }
    return 0;
}

int JLINK_RTTERMINAL_Control(std::uint32_t command,
                             void*         data) {
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    switch(command) {
    case 0:   // start
        {
            f.rttRunning = true;
            for(auto& channel : f.duplexChannels) {
                channel.upFifo.clear();
                channel.downFifo.clear();
            }
            return 0;
        }
    case 1:   // stop
        {
            recordCall("rtt stop");
            f.rttRunning = false;
            return 0;
        }
    case 2:   // getDesc
        {
            if(f.noGetDesc || data == nullptr) { return -1; }
            auto& desc  = *static_cast<RTTBufferDesc*>(data);
            auto  index = static_cast<std::size_t>(desc.bufferIndex);
            desc.name.fill('\0');
            desc.sizeOfBuffer = FakeBufferCapacity;
            desc.flags        = 0;

            std::string name;
            if(desc.direction == 0) {
                if(index >= f.numUpBuffers()) { return -1; }
                name = index < f.numLogBuffers ? "uc_log" + std::to_string(index)
                                               : f.duplexChannels[index - f.numLogBuffers].name;
            } else {
                if(index >= f.duplexChannels.size()) { return -1; }
                name = f.duplexChannels[index].name;
            }
            std::memcpy(desc.name.data(),
                        name.c_str(),
                        std::min(name.size() + 1, desc.name.size() - 1));
            return 0;
        }
    case 4:   // getStatus
        {
            if(data == nullptr) { return -1; }
            auto& status               = *static_cast<RTTStatus*>(data);
            status                     = RTTStatus{};
            status.isRunning           = f.rttRunning ? 1 : 0;
            status.numUpBuffers        = static_cast<int>(f.numUpBuffers());
            status.numDownBuffers      = static_cast<int>(f.duplexChannels.size());
            status.numBytesRead        = f.bytesReadByHost;
            status.numBytesTransferred = f.bytesReadByHost;
            return 0;
        }
    default: return -1;
    }
}

int JLINK_RTTERMINAL_Read(std::uint32_t bufferIndex,
                          char*         buffer,
                          std::uint32_t bufferSize) {
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    if(!f.rttRunning || bufferIndex >= f.numUpBuffers()) { return -1; }
    if(bufferIndex < f.numLogBuffers) { return 0; }   // log buffers stay silent

    auto&      fifo = f.duplexChannels[bufferIndex - f.numLogBuffers].upFifo;
    auto const n    = std::min<std::size_t>(fifo.size(), bufferSize);
    if(n != 0) {
        std::memcpy(buffer, fifo.data(), n);
        fifo.erase(fifo.begin(), std::next(fifo.begin(), static_cast<std::ptrdiff_t>(n)));
        f.bytesReadByHost += static_cast<std::uint32_t>(n);
    }
    return static_cast<int>(n);
}

int JLINK_RTTERMINAL_Write(std::uint32_t bufferIndex,
                           char const*   buffer,
                           std::uint32_t bufferSize) {
    auto&                             f = fake();
    std::lock_guard<std::mutex> const lock{f.mutex};
    if(!f.rttRunning || bufferIndex >= f.duplexChannels.size()) { return -1; }

    // partial writes when the fake down buffer is full, like a real full target buffer
    auto&      fifo = f.duplexChannels[bufferIndex].downFifo;
    auto const n    = std::min<std::size_t>(FakeBufferCapacity - fifo.size(), bufferSize);
    if(n != 0) {
        fifo.insert(fifo.end(), buffer, std::next(buffer, static_cast<std::ptrdiff_t>(n)));
    }
    return static_cast<int>(n);
}
}

std::vector<std::pair<std::uint32_t,
                      std::uint32_t>>
fakeJLinkWrites() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    return writes;
}

void fakeJLinkClearWrites() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    writes.clear();
}

std::vector<std::string> fakeJLinkCalls() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    return calls;
}

void fakeJLinkClearCalls() {
    std::lock_guard<std::mutex> const lock{writesMutex};
    calls.clear();
}
