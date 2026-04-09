/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "CartRetailIR.h"
#include "../NDS.h"
#include "../Utils.h"

// CartRetailIR: NDS cartridge with IR transceiver (Pokémon games)
// the IR transceiver is connected to the SPI interface, with a passthrough command for SRAM access

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace NDSCart
{

CartRetailIR::CartRetailIR(const u8* rom, u32 len, u32 chipid, u32 irversion, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata) :
    CartRetailIR(CopyToUnique(rom, len), len, chipid, irversion, badDSiDump, romparams, std::move(sram), sramlen, userdata)
{
}

CartRetailIR::CartRetailIR(
    std::unique_ptr<u8[]>&& rom,
    u32 len,
    u32 chipid,
    u32 irversion,
    bool badDSiDump,
    ROMListEntry romparams,
    std::unique_ptr<u8[]>&& sram,
    u32 sramlen,
    void* userdata
) :
    CartRetail(std::move(rom), len, chipid, badDSiDump, romparams, std::move(sram), sramlen, userdata, CartType::RetailIR),
    IRVersion(irversion)
{
}

CartRetailIR::~CartRetailIR() = default;

void CartRetailIR::Reset()
{
    CartRetail::Reset();
    Platform::IRClose();
    IRCmd = 0;
}

void CartRetailIR::DoSavestate(Savestate* file)
{
    CartRetail::DoSavestate(file);

    file->Var8(&IRCmd);
}

void CartRetailIR::SPISelect()
{
    CartRetail::SPISelect();

    // If we were in write mode and have data, send it now (SPI deselect means transmission complete)
    if (IRCmd == 0x02 && IRPos > 1)
    {
        u8 sendLen = IRPos - 1; // IRPos includes the command byte
        SendIR(sendLen);
    }

    IRPos = 0;
}

u8 CartRetailIR::SPITransmitReceive(u8 val)
{
    if (IRPos == 0)
    {
        IRCmd = val;
        IRPos++;
        return 0;
    }

    u8 ret = 0;
    switch (IRCmd)
    {
    case 0x00: // pass-through
        ret = CartRetail::SPITransmitReceive(val);
        break;

    case 0x01: // Read from IR
        if (IRPos == 1)
        {
            memset(RxBuf, 0, sizeof(RxBuf)); // May not be needed
            // Initiates the Read. A whole packet will be grabbed by the frontend
            // with this call and stored in RxBuf. The return value is the length
            // of the packet and will tell the GAME to keep sending SPI commands.
            ret = ReadIR();
        }
        else
        {
            // We start returning actual packet data to the game now
            ret = (unsigned char)RxBuf[IRPos - 2];
        }
        break;

    case 0x02: // Write to IR
        TxBuf[IRPos - 1] = val; // Load spi data into Tx Buffer
        // Note: 'last' parameter not available in new SPI interface
        // We'll handle this differently - check if we've received a full packet
        ret = 0x00; // Return value on success
        break;

    case 0x08: // ID
        ret = 0xAA;
        break;
    }

    IRPos++;
    return ret;
}

/*
   This is convoluted because 1: I haven't rewritten it to be nice and 2: We need to wait 3500us for no data.
   If we do NOT wait, walker emulators may work, but real hardware won't. Precise timings should be handled
   HERE to make Platform.h implementations as simple as possible.
*/
u8 CartRetailIR::ReadIR()
{
    char tempBuf[0xB8];
    int len = Platform::IRReceivePacket(tempBuf, sizeof(tempBuf), UserData);
    u8 pointer = 0;
    long long lastRxTime = Platform::GetUSCount();

    // This enters the recieve loop. IF there are bytes to be recieved, keep trying to recieve
    if (len > 0)
    {
        lastRxTime = Platform::GetUSCount();
        for (int i = 0; i < len; i++)
        {
            RxBuf[pointer + i] = tempBuf[i];
        }
        pointer = pointer + len;

        // keep trying to Rx until 3500us has passed
        while ((Platform::GetUSCount() - lastRxTime) < 3500)
        {
            len = Platform::IRReceivePacket(tempBuf, sizeof(tempBuf), UserData);
            if (len <= 0) continue;
            else
            {
                lastRxTime = Platform::GetUSCount();
                for (int i = 0; i < len; i++) RxBuf[pointer + i] = tempBuf[i];
                pointer = pointer + len;
                break;
            }
        }
    }

    recvLen = pointer;
    if (recvLen == 0) return 0;

    /*
    printf("\nRecieved %d Bytes \n", recvLen);
    for (int i = 0; i < recvLen; i++){
        printf("0x%02x ", (u8)buf[i] ^ 0xaa);
    }
    printf("\n");
    */
    return recvLen;
}

// Sends an entire packet to the frontend.
u8 CartRetailIR::SendIR(u8 len)
{
    int sent;
    // Immediate disconnect. This packet needs to WAIT or else it will be piggybacked onto the latest packet (on the pokewalker's end)
    if ((u8)TxBuf[0] == 94) Platform::Sleep(10000);

    sent = Platform::IRSendPacket(TxBuf, len, UserData);
    if (sent < 0) perror("send error");

    return 0;
}


}

}
