#pragma once
#include "jlink/JLinkDLL.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct JLink;

static JLink*& getJLinkInstance() {
    static JLink* instance = nullptr;
    return instance;
}

struct JLink {
public:
    using Status = RTTStatus;

    JLink(JLink const&)            = delete;
    JLink& operator=(JLink const&) = delete;
    JLink(JLink&&)                 = delete;
    JLink& operator=(JLink&&)      = delete;

private:
    std::function<void(std::string_view)> logMsgFunction;
    std::function<void(std::string_view)> errorMsgFunction;

public:
    /// One word written to the target before every reset and download (setPreResetCommands).
    struct MemoryWrite {
        std::uint32_t address{};
        std::uint32_t value{};

        bool operator==(MemoryWrite const&) const = default;
    };

private:
    bool                     rttOpen{false};
    bool                     captureFlashErrors_{false};
    bool                     flashErrorCaptured_{false};
    std::vector<MemoryWrite> preResetWrites_{};

    void log(char const* msg,
             bool        isError) {
        if(isError) {
            if(errorMsgFunction) { errorMsgFunction(std::string_view{msg}); }
            if(captureFlashErrors_) { flashErrorCaptured_ = true; }
        } else {
            if(logMsgFunction) { logMsgFunction(std::string_view{msg}); }
        }
    }

    void connect(std::string const& device,
                 std::uint32_t      speed) {
        if(JLINK_IsOpen() != 0) {
            throw std::runtime_error{std::string{"JLINK_IsOpen: Is allready opened"}};
        }
        preOpenDisableDialogs();
        {
            char const* ret = JLINK_OpenEx(
              [](char const* msg) {
                  if(getJLinkInstance() != nullptr) { getJLinkInstance()->log(msg, false); }
              },
              [](char const* msg) {
                  if(getJLinkInstance() != nullptr) { getJLinkInstance()->log(msg, true); }
              });
            if(ret != nullptr) {
                throw std::runtime_error{std::string{"JLINK_OpenEx failed: "} + ret};
            }
        }
        logProbe();

        preConnectDisableDialogs();
        {
            int const ret = JLINK_TIF_Select(1);   // SWD
            if(ret != 0) {
                JLINK_Close();
                throw std::runtime_error{"JLINK_TIF_Select failed: " + std::to_string(ret)};
            }
        }

        JLINK_SetSpeed(speed);
        execCommand("device = " + device);
        {
            char const ret = JLINK_IsConnected();
            if(ret == 0) {
                int const ret2 = JLINK_Connect();
                if(ret2 != 0) {
                    JLINK_Close();
                    throw std::runtime_error{"JLINK_Connect failed: " + std::to_string(ret2)};
                }
            }
        }
        // dummy to force connect
        JLINK_IsHalted();

        std::size_t tries = 10;

        while(tries != 0) {
            char const ret = JLINK_IsConnected();
            if(ret == 0) {
                --tries;
            } else if(ret == 1) {
                break;
            } else {
                JLINK_Close();
                throw std::runtime_error{"JLINK_IsConnected failed: "
                                         + std::to_string(static_cast<int>(ret))};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if(tries == 0) {
            JLINK_Close();
            throw std::runtime_error{"JLINK_IsConnected failed: timeout"};
        }
        postConnectDisableDialogs();
    }

    void execCommand(std::string const& cmd) {
        {
            std::array<char, 1024> error_buffer{};

            int const ret
              = JLINK_ExecCommand(cmd.c_str(), error_buffer.data(), error_buffer.size());
            if(ret != 0) {
                std::string error_msg;
                if(ret > 0) {
                    error_msg
                      = std::string_view{error_buffer.data(), std::next(error_buffer.data(), ret)};
                }
                JLINK_Close();
                throw std::runtime_error{"JLINK_ExecCommand(\"" + cmd + "\") failed: " + error_msg};
            }
        }
    }

    void checkError(bool doClose = true) {
        int const ret = JLINK_HasError();
        if(ret != 0) {
            if(doClose) { JLINK_Close(); }
            throw std::runtime_error{"JLINK_HasError: " + std::to_string(ret)};
        }
    }

    void preOpenDisableDialogs() { execCommand("SuppressGUI 1"); }

    // One line about the probe that was opened, so a log names the J-Link it came from.
    void logProbe() {
        if(!logMsgFunction) { return; }
        std::array<char, 256> product{};
        std::array<char, 256> firmware{};
        JLINK_EMU_GetProductName(product.data(), static_cast<std::uint32_t>(product.size() - 1));
        JLINK_GetFirmwareString(firmware.data(), static_cast<int>(firmware.size() - 1));
        int const hw = JLINK_GetHardwareVersion();
        logMsgFunction(std::format("{} SN {} HW {}.{:02} FW {}",
                                   cstr(product),
                                   JLINK_GetSN(),
                                   hw / 10000 % 100,
                                   hw / 100 % 100,
                                   cstr(firmware)));
    }

    void preConnectDisableDialogs() {
        execCommand("SilentUpdateFW");
        execCommand("HideDeviceSelection 1");
        execCommand("SuppressControlPanel");
        execCommand("DisableInfoWinFlashDL");
        execCommand("DisableInfoWinFlashBPs");
    }

    void postConnectDisableDialogs() { /*execCommand("SetBatchMode 1");*/ }

    void closeRtt() {
        int const ret = JLINK_RTTERMINAL_Control(1, nullptr);   // stop

        if(ret < 0) {
            throw std::runtime_error{"JLINK_RTTERMINAL_Control failed: " + std::to_string(ret)};
        }
        rttOpen = false;
    }

    template<typename LogF,
             typename ErrorF,
             typename SelectF>
    void init(std::string const& device,
              std::uint32_t      speed,
              LogF&&             logFunction,
              ErrorF&&           errorFunction,
              SelectF&&          selectFunction) {
        logMsgFunction = std::function<void(std::string_view)>{std::forward<LogF>(logFunction)};
        errorMsgFunction
          = std::function<void(std::string_view)>{std::forward<ErrorF>(errorFunction)};
        getJLinkInstance() = this;
        std::invoke(std::forward<SelectF>(selectFunction));
        connect(device, speed);
        checkError();
    }

public:
    // Which probe. A host means J-Link over IP at that address; otherwise `probe` names
    // the J-Link by serial number or nickname (JLinkExe -USB takes the same two), looked
    // for on USB first and then among the J-Links on the network. Empty means "the only
    // one on USB". Two probes on the bus and no name is an error, not a guess: the wrong
    // board's log looks exactly like the right one's.
    struct Connection {
        std::string   host{};
        std::uint16_t port{19020};
        std::string   probe{};
    };

    // The probes the DLL sees on the given host interfaces (EmuHostUsb, EmuHostIp or
    // both), for the selection error messages and for tools that want to list them.
    // Listing IP probes is a UDP discovery and takes a moment.
    static std::vector<EmuConnectInfo> probes(int hostInterfaces) {
        int const n = JLINK_EMU_GetList(hostInterfaces, nullptr, 0);
        if(n < 0) { throw std::runtime_error{"JLINK_EMU_GetList failed: " + std::to_string(n)}; }
        std::vector<EmuConnectInfo> infos(static_cast<std::size_t>(n));
        if(infos.empty()) { return infos; }
        int const filled = JLINK_EMU_GetList(hostInterfaces, infos.data(), n);
        if(filled < 0) {
            throw std::runtime_error{"JLINK_EMU_GetList failed: " + std::to_string(filled)};
        }
        infos.resize(std::min(static_cast<std::size_t>(filled), infos.size()));
        return infos;
    }

    static std::vector<EmuConnectInfo> usbProbes() { return probes(EmuHostUsb); }

    template<std::size_t N>
    static std::string_view cstr(std::array<char,
                                            N> const& chars) {
        auto const end = std::find(chars.begin(), chars.end(), '\0');
        return std::string_view{chars.data(), static_cast<std::size_t>(end - chars.begin())};
    }

    static std::string_view nickNameOf(EmuConnectInfo const& info) { return cstr(info.nickName); }

    static std::string_view productOf(EmuConnectInfo const& info) { return cstr(info.product); }

    // `J-Link EDU Mini 123456 "board-a", J-Link PLUS 654321`, or `none`.
    static std::string describe(std::vector<EmuConnectInfo> const& probes) {
        if(probes.empty()) { return "none"; }
        std::string out;
        for(auto const& p : probes) {
            if(!out.empty()) { out += ", "; }
            auto const product = productOf(p);
            if(!product.empty()) {
                out += product;
                out += ' ';
            }
            out += std::to_string(p.serialNumber);
            auto const nick = nickNameOf(p);
            if(!nick.empty()) {
                out += " \"";
                out += nick;
                out += '"';
            }
        }
        return out;
    }

    // A probe as a tool shows it. `serialNumber` (or `nickName`) is what Connection::probe
    // takes to pick it.
    struct Probe {
        std::uint32_t serialNumber{};
        std::string   product{};
        std::string   nickName{};
        bool          onUsb{};
    };

    // Every J-Link on USB and on the network. The network part is a UDP discovery and
    // takes a moment.
    static std::vector<Probe> listProbes() {
        std::vector<Probe> out;
        for(auto const& p : probes(EmuHostUsb | EmuHostIp)) {
            out.push_back(Probe{p.serialNumber,
                                std::string{productOf(p)},
                                std::string{nickNameOf(p)},
                                (p.connection & EmuHostUsb) != 0});
        }
        return out;
    }

    template<typename LogF,
             typename ErrorF>
    JLink(std::string const& device,
          std::uint32_t      speed,
          Connection const&  connection,
          LogF&&             logFunction,
          ErrorF&&           errorFunction) {
        init(device,
             speed,
             std::forward<LogF>(logFunction),
             std::forward<ErrorF>(errorFunction),
             [&]() {
                 if(!connection.host.empty()) {
                     selectIp(connection.host, connection.port);
                 } else {
                     selectProbe(connection.probe);
                 }
             });
    }

private:
    static void selectIp(std::string const& host,
                         std::uint16_t      port) {
        char const ret = JLINK_SelectIP(host.c_str(), static_cast<int>(port));
        if(ret != 0) {
            throw std::runtime_error{"JLINK_SelectIP(" + host + ", "
                                     + std::to_string(static_cast<unsigned>(port))
                                     + ") failed: " + std::to_string(static_cast<int>(ret))};
        }
    }

    // The probe named by serial number or nickname, or nullptr.
    static EmuConnectInfo const* find(std::vector<EmuConnectInfo> const& probes,
                                      std::string const&                 probe) {
        bool const numeric = !probe.empty() && std::ranges::all_of(probe, [](char c) {
            return c >= '0' && c <= '9';
        });
        for(auto const& p : probes) {
            if((numeric && std::to_string(p.serialNumber) == probe) || nickNameOf(p) == probe) {
                return &p;
            }
        }
        return nullptr;
    }

    static void selectUsb(EmuConnectInfo const& p) {
        int const ret = JLINK_EMU_SelectByUSBSN(p.serialNumber);
        if(ret < 0) {
            throw std::runtime_error{"JLINK_EMU_SelectByUSBSN(" + std::to_string(p.serialNumber)
                                     + ") failed: " + std::to_string(ret)};
        }
    }

    // USB first; a name that is not on USB is then looked for among the J-Links on the
    // network (by serial number or nickname, selected by serial number).
    static void selectProbe(std::string const& probe) {
        auto const usb = usbProbes();

        if(probe.empty()) {
            if(usb.empty()) { throw std::runtime_error{"No JLink devices connected"}; }
            if(usb.size() > 1) {
                throw std::runtime_error{
                  "more than one J-Link on USB, name the one to use (serial number or "
                  "nickname): "
                  + describe(usb)};
            }
            selectUsb(usb.front());
            return;
        }

        if(auto const* p = find(usb, probe); p != nullptr) {
            selectUsb(*p);
            return;
        }

        auto const ip = probes(EmuHostIp);
        if(auto const* p = find(ip, probe); p != nullptr) {
            JLINK_EMU_SelectIPBySN(p->serialNumber);   // reports nothing, the open fails
            return;
        }

        throw std::runtime_error{"no J-Link \"" + probe + "\" found, on USB: " + describe(usb)
                                 + ", on the network: " + describe(ip)};
    }

public:
    ~JLink() noexcept {
        try {
            if(rttOpen) { closeRtt(); }
            JLINK_Close();
            getJLinkInstance() = nullptr;
        } catch(...) {}
    }

    // buffers is the total buffer count (up + down)
    RTTStatus startRtt(std::uint32_t buffers,
                       std::uint32_t configBlockAddress = 0) {
        auto config = [this](std::uint32_t address) {
            if(rttOpen) { closeRtt(); }

            RTTStart start{};
            start.configBlockAddress = address;
            int const ret            = JLINK_RTTERMINAL_Control(0, &start);   // start

            if(ret < 0) {
                throw std::runtime_error{"JLINK_RTTERMINAL_Control failed: " + std::to_string(ret)};
            }
            rttOpen = true;
        };
        RTTStatus status{};
        auto      connectRtt = [&]() {
            std::size_t tries{100};
            status = readStatus();
            while(tries != 0
                  && (status.isRunning == 0
                      || status.numUpBuffers + status.numDownBuffers != static_cast<int>(buffers)))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                status = readStatus();
                checkError();
                --tries;
            }
            return tries != 0;
        };
        config(configBlockAddress);
        if(!connectRtt() && configBlockAddress != 0) {
            config(0);
            if(!connectRtt()) {
                throw std::runtime_error{"JLINK_RTTERMINAL_Control failed: timeout"};
            }
        }
        return status;
    }

    std::span<std::byte> rttRead(std::uint32_t        bufferNumber,
                                 std::span<std::byte> buffer) {
        int const ret = JLINK_RTTERMINAL_Read(bufferNumber,
                                              reinterpret_cast<char*>(buffer.data()),
                                              static_cast<std::uint32_t>(buffer.size()));
        if(ret < 0) {
            throw std::runtime_error{"JLINK_RTTERMINAL_Read failed: " + std::to_string(ret)};
        }
        return buffer.subspan(0, static_cast<std::size_t>(ret));
    }

    // returns the number of bytes accepted, partial writes are normal when the
    // target side down buffer is full
    std::size_t rttWrite(std::uint32_t              bufferNumber,
                         std::span<std::byte const> buffer) {
        int const ret = JLINK_RTTERMINAL_Write(bufferNumber,
                                               reinterpret_cast<char const*>(buffer.data()),
                                               static_cast<std::uint32_t>(buffer.size()));
        if(ret < 0) {
            throw std::runtime_error{"JLINK_RTTERMINAL_Write failed: " + std::to_string(ret)};
        }
        return static_cast<std::size_t>(ret);
    }

    struct BufferDesc {
        std::uint32_t index{};
        bool          isDown{};
        std::string   name;
        std::uint32_t size{};
        std::uint32_t flags{};
    };

    // never throws: getDesc support depends on the DLL version, callers fall back on nullopt
    std::optional<BufferDesc> rttBufferDesc(bool          down,
                                            std::uint32_t index) noexcept {
        RTTBufferDesc desc{};
        desc.bufferIndex = static_cast<int>(index);
        desc.direction   = down ? 1U : 0U;
        try {
            int const ret = JLINK_RTTERMINAL_Control(2, &desc);   // getDesc
            if(ret < 0) { return std::nullopt; }
        } catch(...) { return std::nullopt; }
        auto const nameEnd = std::find(desc.name.begin(), desc.name.end(), '\0');
        return BufferDesc{
          index,
          down,
          std::string{desc.name.begin(), nameEnd},
          desc.sizeOfBuffer,
          desc.flags
        };
    }

    void checkConnected() {
        checkError(false);
        char const ret = JLINK_IsConnected();
        if(ret == 0) {
            throw std::runtime_error{"JLINK_IsConnected: " + std::to_string(static_cast<int>(ret))};
        }
    }

    /// Retries a failed poll a few times before throwing. Under heavy traffic on the host's USB
    /// controller a single call can fail and the next one succeed; dropping the session at once
    /// forces a reconnect, which the DLL may answer by resetting the target via nRESET.
    bool isHalted() {
        static constexpr int Tries = 4;
        for(int attempt = 1;; ++attempt) {
            char const ret = JLINK_IsHalted();
            if(ret >= 0) { return ret > 0; }
            if(attempt >= Tries) {
                throw std::runtime_error{"JLINK_IsHalted: " + std::to_string(static_cast<int>(ret))
                                         + " (" + std::to_string(Tries) + " tries)"};
            }
            if(errorMsgFunction) {
                errorMsgFunction("JLINK_IsHalted: " + std::to_string(static_cast<int>(ret))
                                 + ", try " + std::to_string(attempt) + " of "
                                 + std::to_string(Tries) + " -- the session is kept");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    void setResetType(std::uint8_t type) {
        if(type > 2) {
            throw std::runtime_error{"Invalid reset type: " + std::to_string(static_cast<int>(type))
                                     + " (valid: 0=Normal, 1=Core, 2=ResetPin)"};
        }
        int const ret = JLINK_SetResetType(type);
        if(ret < 0) { throw std::runtime_error{"JLINK_SetResetType: " + std::to_string(ret)}; }
    }

    /// Parses one J-Link Commander line of the subset the printer runs before a reset:
    /// `w4 <address> <value>`, numbers in hex (0x...) or decimal. The chip package's
    /// TARGET_JLINK_CONNECT_COMMANDS come in this form (the RP chips park core 1 with them);
    /// anything else throws, so a line the printer cannot run is an error at startup and not
    /// a reset that silently skips it.
    static MemoryWrite parseCommand(std::string_view line) {
        auto const fail = [&](std::string_view why) {
            return std::runtime_error{"pre-reset command \"" + std::string{line} + "\": "
                                      + std::string{why} + " (only \"w4 <address> <value>\")"};
        };
        std::vector<std::string_view> words;
        for(std::size_t pos = 0; pos < line.size();) {
            pos = line.find_first_not_of(" \t", pos);
            if(pos == std::string_view::npos) { break; }
            auto const end = std::min(line.find_first_of(" \t", pos), line.size());
            words.push_back(line.substr(pos, end - pos));
            pos = end;
        }
        if(words.size() != 3 || (words[0] != "w4" && words[0] != "W4")) {
            throw fail("not a w4 command");
        }
        auto const number = [&](std::string_view word) {
            std::uint64_t base = 10;
            if(word.starts_with("0x") || word.starts_with("0X")) {
                word.remove_prefix(2);
                base = 16;
            }
            auto const bad
              = [&]() { return fail("\"" + std::string{word} + "\" is not a 32 bit number"); };
            if(word.empty()) { throw bad(); }
            std::uint64_t value{};
            for(char const c : word) {
                std::uint64_t digit{};
                if(c >= '0' && c <= '9') {
                    digit = static_cast<std::uint64_t>(c - '0');
                } else if(base == 16 && c >= 'a' && c <= 'f') {
                    digit = static_cast<std::uint64_t>(c - 'a' + 10);
                } else if(base == 16 && c >= 'A' && c <= 'F') {
                    digit = static_cast<std::uint64_t>(c - 'A' + 10);
                } else {
                    throw bad();
                }
                value = value * base + digit;
                if(value > 0xFFFF'FFFFU) { throw bad(); }
            }
            return static_cast<std::uint32_t>(value);
        };
        return MemoryWrite{number(words[1]), number(words[2])};
    }

    /// Words written, in order, before every resetTarget() and flash(): JLINK_Reset and the
    /// reset inside a download restart only the connected core, and a chip with a second core
    /// may need it stopped first. Which words is the chip package's business, not this class's.
    void setPreResetCommands(std::vector<MemoryWrite> writes) {
        preResetWrites_ = std::move(writes);
    }

    void runPreResetCommands() {
        if(preResetWrites_.empty()) { return; }
        std::string rets;
        for(auto const& w : preResetWrites_) {
            rets += " " + std::to_string(JLINK_WriteU32(w.address, w.value));
        }
        if(logMsgFunction) {
            logMsgFunction("pre-reset commands: " + std::to_string(preResetWrites_.size())
                           + " written (rets" + rets + ")");
        }
    }

    void resetTarget() {
        runPreResetCommands();
        int const ret = JLINK_Reset();
        if(ret < 0) { throw std::runtime_error{"JLINK_Reset: " + std::to_string(ret)}; }
    }

    /// Reads target memory while the core keeps running.
    void readMemory(std::uint32_t        address,
                    std::span<std::byte> out) {
        int const ret = JLINK_ReadMem(address, static_cast<std::uint32_t>(out.size()), out.data());
        if(ret != 0) { throw std::runtime_error{"JLINK_ReadMem: " + std::to_string(ret)}; }
    }

    /// Cortex-M core registers by DLL index; only meaningful while the core is halted. The
    /// indices were verified on RP2040 and RP2350 halted in a fault, not against SEGGER's docs.
    enum class CoreRegister : int { sp = 13, lr = 14, pc = 15, xpsr = 16, msp = 17, psp = 18 };

    std::uint32_t readRegister(CoreRegister r) { return JLINK_ReadReg(static_cast<int>(r)); }

    void halt() { JLINK_Halt(); }

    void go() { JLINK_Go(); }

    void clearAllBreakpoints() {
        int const ret = JLINK_ClrBPEx(0xFFFFFFFF);
        if(ret < 0) { throw std::runtime_error{"JLINK_ClrBPEx: " + std::to_string(ret)}; }
    }

    void flash(std::string const& hexFile) {
        runPreResetCommands();
        captureFlashErrors_ = true;
        flashErrorCaptured_ = false;
        int const ret       = JLINK_DownloadFile(hexFile.c_str(), 0);
        captureFlashErrors_ = false;
        if(logMsgFunction) { logMsgFunction("JLINK_DownloadFile ret: " + std::to_string(ret)); }
        if(ret < 0 || flashErrorCaptured_) {
            throw std::runtime_error{"JLINK_DownloadFile failed: " + std::to_string(ret)};
        }
    }

    RTTStatus readStatus() {
        RTTStatus status{};

        int const ret = JLINK_RTTERMINAL_Control(4, &status);   // get status
        if(ret < 0) {
            throw std::runtime_error{"JLINK_RTTERMINAL_Control failed: " + std::to_string(ret)};
        }
        return status;
    }
};
