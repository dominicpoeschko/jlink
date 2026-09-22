#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
struct RTTStart {
    std::uint32_t                configBlockAddress{};
    std::array<std::uint32_t, 3> padding{};
};

struct RTTStatus {
    std::uint32_t numBytesTransferred{};
    std::uint32_t numBytesRead{};
    int           hostOverflowCount{};
    int           isRunning{};
    int           numUpBuffers{};
    int           numDownBuffers{};
    std::uint32_t overflowMask{};
    std::uint32_t padding{};
};

struct RTTBufferDesc {
    int                  bufferIndex{};
    std::uint32_t        direction{};   // 0 = up (target -> host), 1 = down (host -> target)
    std::array<char, 32> name{};
    std::uint32_t        sizeOfBuffer{};
    std::uint32_t        flags{};
};

// JLINKARM_EMU_CONNECT_INFO, one per probe the DLL can see: 264 bytes. The layout is the
// one pylink (pylink/structs.py, JLinkConnectInfo) uses, and it matches what a real DLL
// writes for two probes on USB (serial numbers at 0 and 264, product at 50, nickname at
// 82). The static_asserts pin every offset so a layout change in a future DLL fails the
// build instead of silently misreading the nickname.
struct EmuConnectInfo {
    std::uint32_t                serialNumber{};
    std::uint8_t                 connection{};   // EmuHostUsb / EmuHostIp
    std::uint32_t                usbAddr{};
    std::array<std::uint8_t, 16> ipAddr{};
    int                          time{};      // ms until the UDP discover answer (IP)
    std::uint64_t                time_us{};   // same in us
    std::uint32_t                hwVersion{};
    std::array<std::uint8_t, 6>  macAddr{};
    std::array<char, 32>         product{};
    std::array<char, 32>         nickName{};
    std::array<char, 112>        fwString{};
    char                         isDhcpAssignedIp{};
    char                         isDhcpAssignedIpIsValid{};
    char                         numIpConnections{};
    char                         numIpConnectionsIsValid{};
    std::array<std::uint8_t, 34> padding{};
};

static_assert(sizeof(EmuConnectInfo) == 264,
              "JLINKARM_EMU_CONNECT_INFO is 264 bytes");
static_assert(offsetof(EmuConnectInfo,
                       serialNumber)
              == 0);
static_assert(offsetof(EmuConnectInfo,
                       connection)
              == 4);
static_assert(offsetof(EmuConnectInfo,
                       usbAddr)
              == 8);
static_assert(offsetof(EmuConnectInfo,
                       ipAddr)
              == 12);
static_assert(offsetof(EmuConnectInfo,
                       time)
              == 28);
static_assert(offsetof(EmuConnectInfo,
                       time_us)
              == 32);
static_assert(offsetof(EmuConnectInfo,
                       hwVersion)
              == 40);
static_assert(offsetof(EmuConnectInfo,
                       macAddr)
              == 44);
static_assert(offsetof(EmuConnectInfo,
                       product)
              == 50);
static_assert(offsetof(EmuConnectInfo,
                       nickName)
              == 82);
static_assert(offsetof(EmuConnectInfo,
                       fwString)
              == 114);
static_assert(offsetof(EmuConnectInfo,
                       isDhcpAssignedIp)
              == 226);
static_assert(offsetof(EmuConnectInfo,
                       padding)
              == 230);

// Host interface bits for JLINK_EMU_GetList and EmuConnectInfo::connection.
inline constexpr int EmuHostUsb = 1;
inline constexpr int EmuHostIp  = 2;

int JLINK_EMU_GetNumDevices();
// hostInterfaces: EmuHostUsb, EmuHostIp or both. Returns the number of probes found (may
// exceed maxInfos; pass nullptr, 0 to just count), negative on error.
int JLINK_EMU_GetList(int             hostInterfaces,
                      EmuConnectInfo* infos,
                      int             maxInfos);
// Choose the probe the next JLINK_OpenEx opens. SelectByUSBSN is negative on error;
// SelectIPBySN reports nothing, the open fails instead.
int  JLINK_EMU_SelectByUSBSN(std::uint32_t serialNumber);
void JLINK_EMU_SelectIPBySN(std::uint32_t serialNumber);
// About the opened probe (valid after JLINK_OpenEx).
std::uint32_t JLINK_GetSN();
void          JLINK_EMU_GetProductName(char*         buffer,
                                       std::uint32_t bufferSize);
int           JLINK_GetHardwareVersion();   // major * 10000 + minor * 100
void          JLINK_GetFirmwareString(char* buffer,
                                      int   bufferSize);
char const*   JLINK_OpenEx(void (*log)(char const*),
                           void (*errorLog)(char const*));
char          JLINK_IsOpen();
int           JLINK_TIF_Select(int interface);   // JTAG 0 SWD 1
void          JLINK_SetSpeed(std::uint32_t Speed);
char          JLINK_IsConnected();
int           JLINK_Connect();
char          JLINK_IsHalted();
// Works while the core runs. Returns 0 on success.
int JLINK_ReadMem(std::uint32_t address,
                  std::uint32_t numBytes,
                  void*         data);
// Works while the core runs. Returns 0 on success.
int           JLINK_WriteU32(std::uint32_t address,
                             std::uint32_t data);
std::uint32_t JLINK_ReadReg(int registerIndex);
void          JLINK_Halt();
void          JLINK_Go();
int           JLINK_ClrBPEx(unsigned handle);   // Use 0xFFFFFFFF to clear all
int           JLINK_ExecCommand(char const* in,
                                char*       out,
                                int         bufferSize);
int           JLINK_HasError();
void          JLINK_Close();
char          JLINK_SelectUSB(int port);
char          JLINK_SelectIP(char const* host,
                             int         port);
int           JLINK_Reset();
int           JLINK_SetResetType(std::uint8_t ResetType);   // 0=Normal, 1=Core, 2=ResetPin
int           JLINK_DownloadFile(char const*   sFileName,
                                 std::uint32_t Addr);
int JLINK_RTTERMINAL_Control(std::uint32_t command,   // start 0 stop 1 getDesc 2 getStatus 4
                             void*);
int JLINK_RTTERMINAL_Read(std::uint32_t bufferIndex,
                          char*         buffer,
                          std::uint32_t bufferSize);
int JLINK_RTTERMINAL_Write(std::uint32_t bufferIndex,
                           char const*   buffer,
                           std::uint32_t bufferSize);
}
