#ifndef PLATFORMANDROID_H
#define PLATFORMANDROID_H

#include <string>
#include "Platform.h"

namespace melonDS
{
namespace Platform
{

melonDS::Platform::FileHandle* OpenInternalFile(const std::string path, FileMode mode);

    u8 IRSendPacket(char* data, int len, void* userdata);
    u8 IRReceivePacket(char* data, int len, void* userdata);
    void IRLogPacket(char* data, int len, bool isTx, void* userdata);
}
}

#endif
