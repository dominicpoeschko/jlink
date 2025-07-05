#pragma once

#include <array>
#include <cstdint>

extern "C" {

struct RTTStart {
    std::uint32_t                configBlockAddress;
    std::array<std::uint32_t, 3> padding;
};

struct RTTStatus {
    std::uint32_t numBytesTransferred;
    std::uint32_t numBytesRead;
    int           hostOverflowCount;
    int           isRunning;
    int           numUpBuffers;
    int           numDownBuffers;
    std::uint32_t overflowMask;
    std::uint32_t padding;
};

using log_callback = void(char const*);

char const* JLINK_OpenEx(log_callback* log,
                         log_callback* errorLog);
int         JLINK_TIF_Select(int interface);   //JTAG 0 SWD 1
void        JLINK_SetSpeed(std::uint32_t Speed);
char        JLINK_IsConnected();
int         JLINK_Connect();
char        JLINK_IsHalted();
int         JLINK_ExecCommand(char const* in,
                              char*       out,
                              int         bufferSize);
int         JLINK_HasError();
void        JLINK_Close();
char        JLINK_SelectUSB(int port);
char        JLINK_SelectIP(char const* host,
                           int         port);
int         JLINK_Reset();
int         JLINK_DownloadFile(char const*   sFileName,
                               std::uint32_t Addr);
int         JLINK_RTTERMINAL_Control(std::uint32_t command,
                                     void*);   //start 0 getStatus 4
int         JLINK_RTTERMINAL_Read(std::uint32_t bufferIndex,
                                  char*         buffer,
                                  std::uint32_t bufferSize);
}
