#include "IR.h"
#include "AndroidIRHandler.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>

#include "Platform.h"

#define LOG_TAG "IR"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static MelonDSAndroid::AndroidIRHandler* irHandler = nullptr;

namespace melonDS::Platform
{

int irMode = 0;

/**
 * Set IR transport mode
 * @param mode // 0 = None (disabled), 1 = USB Serial, 2 = TCP, 3 = Direct Storage
 */
void setIRMode(int mode)
{
    irMode = mode;
    LOGD("IR mode set: %d", mode);
}

/**
 * Set IR handler instance
 * Called during setup to provide the platform-specific IR handler
 */
void setHandler(MelonDSAndroid::AndroidIRHandler* handler)
{
    ::irHandler = handler;
    LOGD("IR handler set: %p", handler);
}


static bool IROpenSerial()
{

    if (!irHandler->isSerialOpen()) return irHandler->openSerial();
    return irHandler->isSerialOpen();
}

static bool IROpenTCP()
{

    if (!irHandler->isTCPOpen())
    {
        bool result = irHandler->openTCP();
        return result;
    }

    return irHandler->isTCPOpen();
}

u8 IRSendPacketSerial(char* data, int len, void* userdata)
{

    if (!IROpenSerial())
    {
        LOGE("Serial is not Open");
        return 0;
    }

    int written = irHandler->writeSerial(data, len);

     if (written < 0)
     {
        LOGE("Serial write error");
        return 0;
    }

    // TODO irHandler->serialFlush();

    // LOGD("Serial wrote %d bytes", written);
    return static_cast<u8>(written);

}

u8 IRSendPacketTCP(char* data, int len, void* userdata)
{
    if (!IROpenTCP())
    {
        LOGE("TCP connection is not Open");
        return 0;
    }

    int written = irHandler->writeTCP(data, len);
    if (written < 0)
    {
        LOGE("TCP write error");
        return 0;
    }

    return static_cast<u8>(written);
}

u8 IRSendPacket(char* data, int len, void* userdata)
{
    // LOGD("IRSendPacket called: data=%p, len=%d", data, len);

    if (!data || len <= 0)
    {
        LOGE("IRSendPacket: invalid parameters (data=%p, len=%d)", data, len);
        return 0;
    }

    switch (irMode)
    {
        case 0: return 0;
        case 1: return IRSendPacketSerial(data, len, userdata);
        case 2: return IRSendPacketTCP(data, len, userdata);
        default: return 0;
    }

}

u8 IRReceivePacketSerial(char* data, int len, void* userdata)
{
    if (!IROpenSerial())
    {
        LOGE("Serial is not Open");
        return 0;
    }

    int bytesRead = irHandler->readSerial(data, len);

    if (bytesRead < 0)
    {
        LOGE("Serial read error");
        return 0;
    }

    // TODO irHandler->serialFlush();
    
    //LOGD("Serial read %d bytes", bytesRead);
    return static_cast<u8>(bytesRead);
}

u8 IRReceivePacketTCP(char* data, int len, void* userdata)
{
    if (!IROpenTCP())
    {
        LOGE("TCP connection is not Open");
        return 0;
    }

    int bytesRead = irHandler->readTCP(data, len);
    if (bytesRead < 0)
    {
        LOGE("TCP read error");
        return 0;
    }

    return static_cast<u8>(bytesRead);
}

u8 IRReceivePacket(char* data, int len, void* userdata)
{
    // LOGD("IRReceivePacket called: data=%p, len=%d", data, len);

    if (!data || len <= 0)
    {
        LOGE("IRReceivePacket: invalid parameters (data=%p, len=%d)", data, len);
        return 0;
    }


    switch (irMode)
    {
        case 0: return 0;
        case 1: return IRReceivePacketSerial(data, len, userdata);
        case 2: return IRReceivePacketTCP(data, len, userdata);
        default: return 0;
    }
}

void IRLogPacket(char* data, int len, bool isTx, void* userdata)
{
    if (!data || len <= 0) return;

    const char* direction = isTx ? "Tx" : "Rx";

    char hexStr[512] = {0};
    int offset = 0;

    for (int i = 0; i < len && offset < 500; i++)
    {
        offset += snprintf(hexStr + offset, sizeof(hexStr) - offset,
                          "%02X ", static_cast<unsigned char>(data[i]));
    }

    LOGD("IRLogPacket %s: %s", direction, hexStr);
}

} // namespace melonDS::Platform 