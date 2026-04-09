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

enum IRMode
{
    IR_DEFAULT = 0,
    IR_SERIAL = 1,
    IR_TCP = 2,
    IR_ENET = 3,
};


/******************************************************************************
 * UDP Socket (ENet for Desktop to Desktop)
******************************************************************************/
void IRENetInit()
{
    if (enetInited) return;

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
    enet_deinitialize();
    enetInited = false;
    Log(LogLevel::Info, "ENet deinitialized\n");
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
    {
        enetStates[instanceID] = new ENetState();
    }
    ENetState* state = enetStates[instanceID];
    QMutexLocker locker(&state->Mutex);

    if (isServer)
    {
        // SERVER MODE
        if (!state->Host)
        {
            int serverPort = cfg.GetInt("IR.Network.SelfPort");
            ENetAddress address;
            address.host = ENET_HOST_ANY;
            address.port = serverPort;
            if (irMode == IR_DEFAULT) address.port = 7065;

            state->Host = enet_host_create(&address, 16, 2, 0, 0);

            if (!state->Host)
            {
                Log(LogLevel::Error, "ENet server creation failed on port %d\n", address.port);
                return;
            }

            Log(LogLevel::Info, "ENet server on port %d\n", address.port);
        }

        // Process events (accept connections, receive packets)
        ENetEvent event;
        while (enet_host_service(state->Host, &event, 0) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
            {
                state->Peer = event.peer;
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
    else
    {
        // CLIENT MODE
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

        // Process events (connection, disconnection, receive)
        ENetEvent event;
        while (enet_host_service(state->Host, &event, 0) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
            {
                Log(LogLevel::Info, "ENet connected\n");
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                state->Peer = nullptr;
                Log(LogLevel::Info, "ENet disconnected\n");
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                state->RxQueue.push(event.packet);
            }
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

    if (!state->Peer || !state->Host) return 0;

    ENetPacket* packet = enet_packet_create(data, len, ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet) return 0;

    if (enet_peer_send(state->Peer, 0, packet) < 0)
    {
        enet_packet_destroy(packet);
        return 0;
    }

    enet_host_flush(state->Host);

    char stringBuffer[512];
    int offset = 0;
    for (int i = 0; i < len && i < 32; ++i)
    {
        offset += snprintf(stringBuffer + offset, sizeof(stringBuffer) - offset, "%02X ", static_cast<unsigned char>(data[i]));
    }
    Log(LogLevel::Info, "ID %d Sent %d bytes: %s\n", instanceID, len, stringBuffer);

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

    if (!state->Host) return 0;
    if (state->RxQueue.empty()) return 0;

    ENetPacket* packet = state->RxQueue.front();
    state->RxQueue.pop();

    int bytesRead = (packet->dataLength < (size_t)len) ? packet->dataLength : len;
    memcpy(data, packet->data, bytesRead);

    if (bytesRead > 0)
    {
        char stringBuffer[512];
        int offset = 0;
        for (int i = 0; i < bytesRead && i < 32; ++i)
        {
            offset += snprintf(stringBuffer + offset, sizeof(stringBuffer) - offset, "%02X ", static_cast<unsigned char>(data[i]));
        }
        Log(LogLevel::Info, "ID %d Received %d bytes: %s\n", instanceID, bytesRead, stringBuffer);
    }

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
        char stringBuffer[512];
        int offset = 0;
        for (int i = 0; i < bytesWritten && i < 32; ++i)
        {
            offset += snprintf(stringBuffer + offset, sizeof(stringBuffer) - offset, "%02X ", static_cast<unsigned char>(data[i]));
        }
        Log(LogLevel::Info, "Sent %d bytes: %s\n", bytesWritten, stringBuffer);
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
        char stringBuffer[512];
        int offset = 0;
        for (int i = 0; i < bytesRead && i < 32; ++i)
        {
            offset += snprintf(stringBuffer + offset, sizeof(stringBuffer) - offset, "%02X ", static_cast<unsigned char>(data[i]));
        }
        Log(LogLevel::Info, "Received %d bytes: %s\n", bytesRead, stringBuffer);
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

    char stringBuffer[512];
    int offset = 0;
    for (int i = 0; i < bytesWritten; ++i)
    {
        offset += snprintf(stringBuffer + offset, sizeof(stringBuffer) - offset, "%02X", static_cast<unsigned char>(data[i]));
    }
    Log(LogLevel::Info, "Serial Write %lld bytes: %s\n", bytesWritten, stringBuffer);
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

    char stringBuffer[512];
    int offset = 0;
    for (int i = 0; i < bytesRead; ++i)
    {
        offset += snprintf(stringBuffer + offset, sizeof(stringBuffer) - offset,
                           "%02X", static_cast<unsigned char>(data[i]));
    }
    Log(LogLevel::Info, "Serial Read %lld bytes: %s\n", bytesRead, stringBuffer);
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
        case IR_DEFAULT: return IRSendPacketENet(data, len, userdata);
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
        case IR_DEFAULT: return IRReceivePacketENet(data, len, userdata);
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
