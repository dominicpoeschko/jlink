#pragma once
#include "jlink/JLinkDLL.h"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <print>
#include <span>
#include <string>
#include <thread>

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

    bool rttOpen{false};

    void log(char const* msg,
             bool        isError) {
        if(isError) {
            if(errorMsgFunction) { errorMsgFunction(std::string_view{msg}); }
        } else {
            if(logMsgFunction) { logMsgFunction(std::string_view{msg}); }
        }
    }

    void connect(std::string const& device,
                 std::uint32_t      speed) {
        if(JLINK_IsOpen() != 0) {
            throw std::runtime_error{std::string{"JLINK_IsOpen: Is allready opened"}};
        }

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

        {
            int const ret = JLINK_TIF_Select(1);   //SWD
            if(ret != 0) {
                JLINK_Close();
                throw std::runtime_error{"JLINK_TIF_Select failed: " + std::to_string(ret)};
            }
        }

        JLINK_SetSpeed(speed);
        preConnectDisableDialogs();
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
        //dummy to force connect
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

    void preConnectDisableDialogs() {
        execCommand("DisableAutoUpdateFW");
        execCommand("SilentUpdateFW");
        execCommand("SuppressInfoUpdateFW");
        execCommand("HideDeviceSelection 1");
        execCommand("SuppressControlPanel");
        execCommand("DisableInfoWinFlashDL");
        execCommand("DisableInfoWinFlashBPs");
    }

    void postConnectDisableDialogs() { execCommand("SetBatchMode 1"); }

    void closeRtt() {
        int const ret = JLINK_RTTERMINAL_Control(1, nullptr);   //stop

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
    template<typename LogF,
             typename ErrorF>
    JLink(std::string const& device,
          std::uint32_t      speed,
          std::string const& ipAddress,
          LogF&&             logFunction,
          ErrorF&&           errorFunction,
          std::uint16_t      port = 19020) {
        init(device,
             speed,
             std::forward<LogF>(logFunction),
             std::forward<ErrorF>(errorFunction),
             [&]() {
                 char const ret = JLINK_SelectIP(ipAddress.c_str(), static_cast<int>(port));
                 if(ret != 0) {
                     throw std::runtime_error{"JLINK_SelectIP(" + ipAddress + ", "
                                              + std::to_string(static_cast<int>(port))
                                              + ") failed: " + std::to_string(ret)};
                 }
             });
    }

    template<typename LogF,
             typename ErrorF>
    JLink(std::string const& device,
          std::uint32_t      speed,
          LogF&&             logFunction,
          ErrorF&&           errorFunction) {
        init(device,
             speed,
             std::forward<LogF>(logFunction),
             std::forward<ErrorF>(errorFunction),
             []() {
                 char const ret = JLINK_SelectUSB(0);
                 if(ret != 0) {
                     throw std::runtime_error{"JLINK_SelectUSB failed: " + std::to_string(ret)};
                 }
             });
    }

    ~JLink() noexcept {
        try {
            if(rttOpen) { closeRtt(); }
            JLINK_Close();
            getJLinkInstance() = nullptr;
        } catch(...) {}
    }

    void startRtt(std::uint32_t buffers,
                  std::uint32_t configBlockAddress = 0) {
        auto config = [this](std::uint32_t address) {
            if(rttOpen) { closeRtt(); }

            RTTStart start{};
            start.configBlockAddress = address;
            int const ret            = JLINK_RTTERMINAL_Control(0, &start);   //start

            if(ret < 0) {
                throw std::runtime_error{"JLINK_RTTERMINAL_Control failed: " + std::to_string(ret)};
            }
            rttOpen = true;
        };
        auto connectRtt = [&]() {
            std::size_t tries{100};
            RTTStatus   status{readStatus()};
            while(tries != 0
                  && (status.isRunning == 0 || status.numUpBuffers != static_cast<int>(buffers)))
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

    void checkConnected() {
        checkError(false);
        char const ret = JLINK_IsConnected();
        if(ret == 0) {
            throw std::runtime_error{"JLINK_IsConnected: " + std::to_string(static_cast<int>(ret))};
        }
    }

    bool isHalted() {
        char const ret = JLINK_IsHalted();
        if(ret < 0) {
            throw std::runtime_error{"JLINK_IsHalted: " + std::to_string(static_cast<int>(ret))};
        }
        return ret > 0;
    }

    void setResetType(std::uint8_t type) {
        if(type > 2) {
            throw std::runtime_error{"Invalid reset type: " + std::to_string(static_cast<int>(type))
                                     + " (valid: 0=Normal, 1=Core, 2=ResetPin)"};
        }
        int const ret = JLINK_SetResetType(type);
        if(ret < 0) { throw std::runtime_error{"JLINK_SetResetType: " + std::to_string(ret)}; }
    }

    void resetTarget() {
        int const ret = JLINK_Reset();
        if(ret < 0) { throw std::runtime_error{"JLINK_Reset: " + std::to_string(ret)}; }
    }

    void halt() { JLINK_Halt(); }

    void go() { JLINK_Go(); }

    void clearAllBreakpoints() {
        int const ret = JLINK_ClrBPEx(0xFFFFFFFF);
        if(ret < 0) { throw std::runtime_error{"JLINK_ClrBPEx: " + std::to_string(ret)}; }
    }

    void flash(std::string const& hexFile) {
        int const ret = JLINK_DownloadFile(hexFile.c_str(), 0);
        if(ret < 0) { throw std::runtime_error{"JLINK_DownloadFile: " + std::to_string(ret)}; }
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
