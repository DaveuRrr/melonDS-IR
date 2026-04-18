#ifndef PLATFORMANDROID_H
#define PLATFORMANDROID_H

#include <string>
#include "Platform.h"

namespace melonDS
{
namespace Platform
{

melonDS::Platform::FileHandle* OpenInternalFile(const std::string path, FileMode mode);

    u8 IR_SendPacket(char* data, int len, void* userdata);
    u8 IR_ReceivePacket(char* data, int len, void* userdata);
    void IR_LogPacket(char* data, int len, bool isTx, void* userdata);
}
}

#endif
