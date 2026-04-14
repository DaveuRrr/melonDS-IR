#include <stdio.h>
#include <string.h>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMutex>
#include <QThread>
#include <QRandomGenerator>
#include <QtSerialPort/QSerialPort>
#include <queue>
#include <map>
#include <enet/enet.h>

#include "Platform.h"
#include "Config.h"
#include "EmuInstance.h"
#include "MPInterface.h"


namespace melonDS::Platform
{

QSerialPort* Serial = nullptr;
QTcpServer* Server = nullptr;
QTcpSocket* Sock = nullptr;
QMutex NetworkMutex;

struct ENetState {
    ENetHost* Host = nullptr;
    ENetPeer* Peer = nullptr;
    std::queue<ENetPacket*> RxQueue;
    QMutex Mutex;
};
std::map<int, ENetState*> enetStates;
bool enetInited = false;

struct IRLocalQueue {
    std::queue<std::vector<u8>> Queue;
    QMutex Mutex;
};
static IRLocalQueue irLocalQueues[2];

enum IRMode
{
    IR_DEFAULT = 0,
    IR_SERIAL = 1,
    IR_TCP = 2,
    IR_ENET = 3,
};

static std::string IRBytesToString(const char* data, int len)
{
    char buf[512];
    int offset = 0;
    for (int i = 0; i < len && i < 32 && offset < (int)sizeof(buf) - 3; ++i)
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%02X ", (unsigned char)data[i]);
    return std::string(buf, offset);
}

/******************************************************************************
 * IR Local Instances
******************************************************************************/
u8 IRSendPacketLocal(char* data, int len, void* userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    int instanceID = inst->getInstanceID();
    int destID = 1 - instanceID;  // 0->1, 1->0

    if (destID < 0 || destID > 1) return 0;

    QMutexLocker locker(&irLocalQueues[destID].Mutex);
    irLocalQueues[destID].Queue.push(std::vector<u8>((u8*)data, (u8*)data + len));

    Log(LogLevel::Info, "ID %d Local Sent %d bytes: %s\n", instanceID, len, IRBytesToString(data, len).c_str());
    return static_cast<u8>(len);
}

u8 IRReceivePacketLocal(char* data, int len, void* userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    int instanceID = inst->getInstanceID();

    if (instanceID < 0 || instanceID > 1) return 0;

    QMutexLocker locker(&irLocalQueues[instanceID].Mutex);
    if (irLocalQueues[instanceID].Queue.empty()) return 0;

    auto& packet = irLocalQueues[instanceID].Queue.front();
    int bytesRead = std::min((int)packet.size(), len);
    memcpy(data, packet.data(), bytesRead);
    irLocalQueues[instanceID].Queue.pop();

    Log(LogLevel::Info, "ID %d Local Received %d bytes: %s\n", instanceID, bytesRead, IRBytesToString(data, len).c_str());
    return static_cast<u8>(bytesRead);
}

/******************************************************************************
 * IR UDP Socket
******************************************************************************/
void IRENetInit()
{
    if (enetInited) return;

    if (MPInterface::GetType() == MPInterface_LAN)
    {
        enetInited = true;
        Log(LogLevel::Info, "ENet already initialized by LAN\n");
        return;
    }

    if (enet_initialize() != 0)
    {
        Log(LogLevel::Error, "Failed to initialize ENet\n");
        return;
    }

    enetInited = true;
    Log(LogLevel::Info, "ENet initialized\n");
}

void IRENetDeinit()
{
    if (!enetInited) return;

    for (auto& pair : enetStates)
    {
        ENetState* state = pair.second;

        if (state->Peer)
        {
            enet_peer_disconnect_now(state->Peer, 0);
            state->Peer = nullptr;
        }

        if (state->Host)
        {
            enet_host_destroy(state->Host);
            state->Host = nullptr;
        }

        while (!state->RxQueue.empty())
        {
            enet_packet_destroy(state->RxQueue.front());
            state->RxQueue.pop();
        }

        delete state;
    }

    enetStates.clear();
    if (MPInterface::GetType() != MPInterface_LAN)
    {
        enet_deinitialize();
        Log(LogLevel::Info, "ENet deinitialized\n");
        enetInited = false;
    }
}

void IRENetOpen(void* userdata)
{
    IRENetInit();

    EmuInstance* inst = (EmuInstance*)userdata;
    int instanceID = inst->getInstanceID();
    auto& cfg = inst->getLocalConfig();
    int irMode = cfg.GetInt("IR.Mode");
    bool isServer = cfg.GetBool("IR.Network.IsServer");

    if (instanceID > 0) irMode = IR_DEFAULT;
    if (irMode == IR_DEFAULT) isServer = (instanceID == 0);

    if (enetStates.find(instanceID) == enetStates.end())
        enetStates[instanceID] = new ENetState();

    ENetState* state = enetStates[instanceID];
    QMutexLocker locker(&state->Mutex);

    if (isServer)
    {
        if (!state->Host)
        {
            int serverPort = cfg.GetInt("IR.Network.SelfPort");
            ENetAddress address;
            address.host = ENET_HOST_ANY;
            address.port = (irMode == IR_DEFAULT) ? 7065 : serverPort;

            state->Host = enet_host_create(&address, 16, 2, 0, 0);
            if (!state->Host)
            {
                Log(LogLevel::Error, "ENet server creation failed on port %d\n", address.port);
                return;
            }
            Log(LogLevel::Info, "ENet server on port %d\n", address.port);
        }
    }
    else
    {
        if (!state->Host)
        {
            state->Host = enet_host_create(nullptr, 16, 2, 0, 0);
            if (!state->Host)
            {
                Log(LogLevel::Error, "ENet client creation failed\n");
                return;
            }
            Log(LogLevel::Info, "ENet client created\n");
        }

        if (!state->Peer)
        {
            QByteArray hostIP = cfg.GetQString("IR.Network.HostIP").toUtf8();
            int hostPort = cfg.GetInt("IR.Network.HostPort");

            ENetAddress address;
            enet_address_set_host(&address, hostIP.constData());
            address.port = hostPort;
            if (instanceID > 0)
            {
                address.host = 0x0100007F;
                address.port = 7065;
            }

            state->Peer = enet_host_connect(state->Host, &address, 2, 0);
            if (state->Peer) Log(LogLevel::Info, "ENet connecting to %d:%d\n", address.host, address.port);
        }
    }
}

void IRENetProcessEvents(ENetState* state)
{
    if (!state->Host) return;

    ENetEvent event;
    while (enet_host_service(state->Host, &event, 0) > 0)
    {
        if (event.type == ENET_EVENT_TYPE_CONNECT)
        {
            if (!state->Peer) state->Peer = event.peer;
            Log(LogLevel::Info, "ENet peer connected\n");
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            if (state->Peer == event.peer) state->Peer = nullptr;
            Log(LogLevel::Info, "ENet peer disconnected\n");
        }
        else if (event.type == ENET_EVENT_TYPE_RECEIVE)
        {
            state->RxQueue.push(event.packet);
        }
    }
}

u8 IRSendPacketENet(char* data, int len, void* userdata)
{
    IRENetOpen(userdata);

    EmuInstance* inst = (EmuInstance*)userdata;
    int instanceID = inst->getInstanceID();

    if (enetStates.find(instanceID) == enetStates.end()) return 0;
    ENetState* state = enetStates[instanceID];

    QMutexLocker locker(&state->Mutex);

    IRENetProcessEvents(state);

    if (!state->Peer || !state->Host) return 0;

    ENetPacket* packet = enet_packet_create(data, len, ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet) return 0;

    if (enet_peer_send(state->Peer, 0, packet) < 0)
    {
        enet_packet_destroy(packet);
        return 0;
    }

    enet_host_flush(state->Host);

    Log(LogLevel::Info, "ID %d Sent %d bytes: %s\n", instanceID, len, IRBytesToString(data, len).c_str());

    return static_cast<u8>(len);
}

u8 IRReceivePacketENet(char* data, int len, void* userdata)
{
    IRENetOpen(userdata);

    EmuInstance* inst = (EmuInstance*)userdata;
    int instanceID = inst->getInstanceID();

    if (enetStates.find(instanceID) == enetStates.end()) return 0;
    ENetState* state = enetStates[instanceID];

    QMutexLocker locker(&state->Mutex);

    IRENetProcessEvents(state);

    if (state->RxQueue.empty()) return 0;

    ENetPacket* packet = state->RxQueue.front();
    state->RxQueue.pop();

    int bytesRead = (packet->dataLength < (size_t)len) ? packet->dataLength : len;
    memcpy(data, packet->data, bytesRead);

    Log(LogLevel::Info, "ID %d Received %d bytes: %s\n", instanceID, bytesRead, IRBytesToString(data, bytesRead).c_str());

    enet_packet_destroy(packet);

    return static_cast<u8>(bytesRead);
}

/******************************************************************************
 * IR Socket TCP
******************************************************************************/
void IRSocketClose()
{
    QMutexLocker locker(&NetworkMutex);
    if (Sock)
    {
        delete Sock;
        Sock = nullptr;
    }
    if (Server)
    {
        delete Server;
        Server = nullptr;
    }
}

void IRSocketOpen(void* userdata)
{
    QMutexLocker locker(&NetworkMutex);

    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    bool isServer = cfg.GetBool("IR.Network.IsServer");
    int instanceID = inst->getInstanceID();
    if (instanceID > 0) isServer = false;

    if (isServer)
    {
        // SERVER MODE: Create server if needed
        if (!Server)
        {
            Server = new QTcpServer();
            int serverPort = cfg.GetInt("IR.Network.SelfPort");
            if (!Server->listen(QHostAddress::Any, serverPort))
            {
                Log(LogLevel::Error, "Failed to start TCP server on port %d\n", serverPort);
                delete Server;
                Server = nullptr;
                return;
            }
            Log(LogLevel::Info, "TCP server listening on port %d\n", serverPort);
        }

        // Check for disconnected client
        if (Sock && Sock->state() != QAbstractSocket::ConnectedState)
        {
            delete Sock;
            Sock = nullptr;
            Log(LogLevel::Info, "Client disconnected\n");
        }

        // Accept new client if available
        if (!Sock && Server->hasPendingConnections())
        {
            QCoreApplication::processEvents();
            Sock = Server->nextPendingConnection();
            Log(LogLevel::Info, "Client connected from %s\n", Sock->peerAddress().toString().toUtf8().constData());
        }
    }
    else
    {
        // CLIENT MODE: Connect to remote server
        if (!Sock)
        {
            QString hostIP = cfg.GetQString("IR.Network.HostIP");
            int hostPort = cfg.GetInt("IR.Network.HostPort");

            Sock = new QTcpSocket();
            Sock->connectToHost(hostIP, hostPort);

            Log(LogLevel::Info, "TCP client connecting to %s:%d\n", hostIP.toUtf8().constData(), hostPort);

            if (Sock->waitForConnected(10)) Log(LogLevel::Info, "Connected to %s:%d\n", hostIP.toUtf8().constData(), hostPort);

        }
        else if (Sock->state() != QAbstractSocket::ConnectedState)
        {
            delete Sock;
            Sock = nullptr;
            Log(LogLevel::Info, "Disconnected from server\n");
        }
    }
}

u8 IRSendPacketTCP(char* data, int len, void* userdata)
{
    QCoreApplication::processEvents();
    IRSocketOpen(userdata);

    QMutexLocker locker(&NetworkMutex);

    if (!Sock || Sock->state() != QAbstractSocket::ConnectedState) return 0;

    qint64 bytesWritten = Sock->write(data, len);
    Sock->flush();

    if (bytesWritten > 0)
    {
        Log(LogLevel::Info, "Sent %d bytes: %s\n", bytesWritten, IRBytesToString(data, len).c_str());
    }

    return static_cast<u8>(bytesWritten);
}

u8 IRReceivePacketTCP(char* data, int len, void* userdata)
{
    QCoreApplication::processEvents();
    IRSocketOpen(userdata);

    QMutexLocker locker(&NetworkMutex);

    if (!Sock || Sock->bytesAvailable() <= 0) return 0;

    qint64 bytesRead = Sock->read(data, len);

    if (bytesRead > 0)
    {
        Log(LogLevel::Info, "Received %d bytes: %s\n", bytesRead, IRBytesToString(data, bytesRead).c_str());
    }

    return static_cast<u8>(bytesRead);
}


/******************************************************************************
 * IR Serial
******************************************************************************/
bool IRSerial()
{
    if (!Serial || !Serial->isOpen()) return false;

    // Check for Serial port errors that indicate disconnection
    QSerialPort::SerialPortError error = Serial->error();
    if (error == QSerialPort::ResourceError ||
        error == QSerialPort::PermissionError ||
        error == QSerialPort::DeviceNotFoundError) 
    {
        return false;
    }

    return true;
}

void IRSerialClosePort()
{
    if (Serial) 
    {
        if (Serial->isOpen()) Serial->close();
        printf("Serial port closed\n");
        delete Serial;
        Serial = nullptr;
    }
}

void IRSerialOpenPort(void* userdata)
{
    if (!IRSerial()) IRSerialClosePort();

    if (!Serial)
    {
        EmuInstance* inst = (EmuInstance*)userdata;
        auto& cfg = inst->getLocalConfig();
        QString portPath = cfg.GetQString("IR.SerialPortPath");
        Serial = new QSerialPort();
        // Windows: COM ports above COM9 need \\.\COMxx format
        #ifdef _WIN32
        if (portPath.startsWith("COM", Qt::CaseInsensitive))
        {
            int portNum = portPath.mid(3).toInt();
            if (portNum > 9) portPath = "\\\\.\\" + portPath;
        }
        #endif

        Serial->setPortName(portPath);
        Log(LogLevel::Info, "Attempting to open Serial port: %s\n", portPath.toUtf8().constData());

        if (!Serial->open(QIODevice::ReadWrite)) 
        {   
            Log(LogLevel::Error, "Failed to open Serial port %s: %s\n", portPath.toUtf8().constData(), Serial->errorString().toUtf8().constData());
        }
        else {
            // Configure port settings AFTER opening
            Serial->setBaudRate(QSerialPort::Baud115200);
            Serial->setDataBits(QSerialPort::Data8);
            Serial->setParity(QSerialPort::NoParity);
            Serial->setStopBits(QSerialPort::OneStop);
            Serial->setFlowControl(QSerialPort::NoFlowControl);

            // Explicitly set DTR and RTS high for device stability
            Serial->setDataTerminalReady(true);
            // Serial->setRequestToSend(true);
            Serial->clear();
            Log(LogLevel::Info, "Serial port opened successfully: %s (115200 8N1, DTR=1, RTS=0)\n", portPath.toUtf8().constData());
        }
    }
    
    return;
}

u8 IRSendPacketSerial(char* data, int len, void* userdata)
{
    QCoreApplication::processEvents();
    IRSerialOpenPort(userdata);

    if (!Serial || !Serial->isOpen()) 
    {
        Log(LogLevel::Error, "Serial write failed: port not open\n");
        return 0;
    }

    qint64 bytesWritten = Serial->write(data, len);

    if (bytesWritten < 0)
    {
        Log(LogLevel::Error, "Serial write error: %s\n", Serial->errorString().toUtf8().constData());
        return 0;
    }

    Serial->flush();

    Log(LogLevel::Info, "Serial Write %lld bytes: %s\n", bytesWritten, IRBytesToString(data, len).c_str());
    return static_cast<u8>(bytesWritten);
}

u8 IRReceivePacketSerial(char* data, int len,void* userdata)
{
    QCoreApplication::processEvents();
    IRSerialOpenPort(userdata);


    if (!Serial || !Serial->isOpen() || !Serial->bytesAvailable()) return 0;

    qint64 bytesRead = Serial->read(data, len);

    if (bytesRead < 0)
    {
        Log(LogLevel::Error, "Serial read error: %s\n", Serial->errorString().toUtf8().constData());
        return 0;
    }

    Log(LogLevel::Info, "Serial Read %lld bytes: %s\n", bytesRead, IRBytesToString(data, bytesRead).c_str());
    return static_cast<u8>(bytesRead);
}

/******************************************************************************
 * Platform IR Send & Receive
******************************************************************************/
u8 IRSendPacket(char* data, int len, void* userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    int irMode = cfg.GetInt("IR.Mode");

    int instanceID = inst->getInstanceID();
    if (instanceID > 0) irMode = IR_DEFAULT;

    // Log(LogLevel::Info, "ID %d IRSendPacket mode=%d len=%d\n", instanceID, irMode, len);

    if (irMode != IR_SERIAL) IRSerialClosePort();
    if (irMode != IR_TCP) IRSocketClose();

    switch(irMode)
    {
        case IR_DEFAULT: return IRSendPacketLocal(data, len, userdata); //return IRSendPacketENet(data, len, userdata);
        case IR_SERIAL: return IRSendPacketSerial(data, len, userdata);
        case IR_TCP: return IRSendPacketTCP(data, len, userdata);
        case IR_ENET: return IRSendPacketENet(data, len, userdata);
        default: return IR_DEFAULT;
    }
}

u8 IRReceivePacket(char* data, int len, void* userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    int irMode = cfg.GetInt("IR.Mode");

    int instanceID = inst->getInstanceID();
    if (instanceID > 0) irMode = IR_DEFAULT;

    // Log(LogLevel::Info, "ID %d IRReceivePacket mode=%d len=%d\n", instanceID, irMode, len);

    if (irMode != IR_SERIAL) IRSerialClosePort();
    if (irMode != IR_TCP) IRSocketClose();

    switch(irMode)
    {
        case IR_DEFAULT: return IRReceivePacketLocal(data, len, userdata); //return IRReceivePacketENet(data, len, userdata);
        case IR_SERIAL: return IRReceivePacketSerial(data, len, userdata);
        case IR_TCP: return IRReceivePacketTCP(data, len, userdata);
        case IR_ENET: return IRReceivePacketENet(data, len, userdata);
        default: return IR_DEFAULT;
    }
}

void IRClose()
{
    IRENetDeinit();
    IRSocketClose();
    IRSerialClosePort();
}

}
