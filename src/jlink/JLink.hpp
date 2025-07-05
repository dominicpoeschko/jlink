#pragma once
#include "jlink/JLinkDLL.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <thread>

struct JLink {
public:
    using Status = RTTStatus;

    JLink(JLink const&)            = delete;
    JLink& operator=(JLink const&) = delete;
    JLink(JLink&&)                 = delete;
    JLink& operator=(JLink&&)      = delete;

private:
    static void log(char const*) {
        //        std::print(stderr, " jlink_log: {}\n", msg);
    }
    static void log_error(char const* msg) { std::print(stderr, " jlink_log_error: {}\n", msg); }

    void connect(std::string const& device, std::uint32_t speed) {
        {
            char const* ret = JLINK_OpenEx(log, log_error);
            if(ret != nullptr) {
                throw std::runtime_error{std::string{"JLINK_OpenEx failed: "} + ret};
            }
        }

        {
            int const ret = JLINK_TIF_Select(1);   //SWD
            if(ret != 0) {
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
                throw std::runtime_error{
                  "JLINK_IsConnected failed: " + std::to_string(static_cast<int>(ret))};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if(tries == 0) {
            throw std::runtime_error{"JLINK_IsConnected failed: timeout"};
        }
        postConnectDisableDialogs();
    }

    void execCommand(std::string const& cmd) {
        {
            std::array<char, 1024> error_buffer;

            int const ret
              = JLINK_ExecCommand(cmd.c_str(), error_buffer.data(), error_buffer.size());
            if(ret != 0) {
                std::string error_msg;
                if(ret > 0) {
                    error_msg
                      = std::string_view{error_buffer.data(), std::next(error_buffer.data(), ret)};
                }
                throw std::runtime_error{"JLINK_ExecCommand(\"" + cmd + "\") failed: " + error_msg};
            }
        }
    }

    void checkError() {
        int const ret = JLINK_HasError();
        if(ret != 0) {
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

public:
    JLink(
      std::string const& device,
      std::uint32_t      speed,
      std::string const& ip,
      std::uint16_t      port = 19020) {
        {
            int const ret = JLINK_SelectIP(ip.c_str(), port);
            if(ret != 0) {
                throw std::runtime_error{
                  "JLINK_SelectIP(" + ip + ", " + std::to_string(static_cast<int>(port))
                  + ") failed: " + std::to_string(ret)};
            }
        }
        connect(device, speed);
        checkError();
    }

    explicit JLink(std::string const& device, std::uint32_t speed) {
        {
            int const ret = JLINK_SelectUSB(0);
            if(ret != 0) {
                throw std::runtime_error{"JLINK_SelectUSB failed: " + std::to_string(ret)};
            }
        }
        connect(device, speed);
        checkError();
    }

    ~JLink() { JLINK_Close(); }

    void startRtt(std::uint32_t buffers, std::uint32_t configBlockAddress = 0) {
        auto config = [](std::uint32_t address) {
            RTTStart start{};
            start.configBlockAddress = address;
            int const ret            = JLINK_RTTERMINAL_Control(0, &start);   //start

            if(ret < 0) {
                throw std::runtime_error{"JLINK_RTTERMINAL_Control failed: " + std::to_string(ret)};
            }
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

    std::span<std::byte> rttRead(std::uint32_t bufferNumber, std::span<std::byte> buffer) {
        int const ret = JLINK_RTTERMINAL_Read(
          bufferNumber,
          reinterpret_cast<char*>(buffer.data()),
          static_cast<std::uint32_t>(buffer.size()));
        if(ret < 0) {
            throw std::runtime_error{"JLINK_RTTERMINAL_Read failed: " + std::to_string(ret)};
        }
        return buffer.subspan(0, static_cast<std::size_t>(ret));
    }

    void checkConnected() {
        checkError();
        char const ret = JLINK_IsConnected();
        if(ret == 0) {
            throw std::runtime_error{"JLINK_IsConnected: " + std::to_string(static_cast<int>(ret))};
        }
    }

    void resetTarget() {
        int const ret = JLINK_Reset();
        if(ret < 0) {
            throw std::runtime_error{"JLINK_Reset: " + std::to_string(ret)};
        }
    }

    void flash(std::string const& hexFile) {
        int const ret = JLINK_DownloadFile(hexFile.c_str(), 0);
        if(ret < 0) {
            throw std::runtime_error{"JLINK_DownloadFile: " + std::to_string(ret)};
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
