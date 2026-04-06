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

QSerialPort *serial = nullptr;
QTcpServer *server = nullptr;
QTcpSocket *sock = nullptr;
QMutex networkMutex;

struct ENetState {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    std::queue<ENetPacket*> rxQueue;
    QMutex mutex;
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
 * IR ENet (UDP-based networking for Desktop-to-Desktop)
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

        if (state->peer)
        {
            enet_peer_disconnect_now(state->peer, 0);
            state->peer = nullptr;
        }

        if (state->host)
        {
            enet_host_destroy(state->host);
            state->host = nullptr;
        }

        while (!state->rxQueue.empty())
        {
            enet_packet_destroy(state->rxQueue.front());
            state->rxQueue.pop();
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

    bool isServer;
    EmuInstance* inst = (EmuInstance*)userdata;
    int instanceID = inst->getInstanceID();
    auto& cfg = inst->getLocalConfig();
    int irMode = cfg.GetInt("IR.Mode");
    isServer = cfg.GetBool("IR.Network.IsServer");

    if (instanceID > 0) irMode = IR_DEFAULT;
    if (irMode == IR_DEFAULT) isServer = (instanceID == 0);

    if (enetStates.find(instanceID) == enetStates.end())
    {
        enetStates[instanceID] = new ENetState();
    }
    ENetState* state = enetStates[instanceID];
    QMutexLocker locker(&state->mutex);

    if (isServer)
    {
        // SERVER MODE
        if (!state->host)
        {
            int serverPort = cfg.GetInt("IR.Network.SelfPort");
            ENetAddress address;
            address.host = ENET_HOST_ANY;
            address.port = serverPort;
            if (irMode == IR_DEFAULT) address.port = 7065;

            state->host = enet_host_create(&address, 16, 2, 0, 0);

            if (!state->host)
            {
                Log(LogLevel::Error, "ENet server creation failed on port %d\n", address.port);
                return;
            }

            Log(LogLevel::Info, "ENet server on port %d\n", address.port);
        }

        // Process events (accept connections, receive packets)
        ENetEvent event;
        while (enet_host_service(state->host, &event, 0) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
            {
                state->peer = event.peer;
                Log(LogLevel::Info, "ENet peer connected\n");
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                if (state->peer == event.peer) state->peer = nullptr;
                Log(LogLevel::Info, "ENet peer disconnected\n");
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                state->rxQueue.push(event.packet);
            }
        }
    }
    else
    {
        // CLIENT MODE
        if (!state->host)
        {
            state->host = enet_host_create(nullptr, 16, 2, 0, 0);

            if (!state->host)
            {
                Log(LogLevel::Error, "ENet client creation failed\n");
                return;
            }

            Log(LogLevel::Info, "ENet client created\n");
        }

        if (!state->peer)
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

            state->peer = enet_host_connect(state->host, &address, 2, 0);

            if (state->peer) Log(LogLevel::Info, "ENet connecting to %d:%d\n", address.host, address.port);
        }

        // Process events (connection, disconnection, receive)
        ENetEvent event;
        while (enet_host_service(state->host, &event, 0) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
            {
                Log(LogLevel::Info, "ENet connected\n");
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                state->peer = nullptr;
                Log(LogLevel::Info, "ENet disconnected\n");
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                state->rxQueue.push(event.packet);
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

    QMutexLocker locker(&state->mutex);

    if (!state->peer || !state->host) return 0;

    ENetPacket* packet = enet_packet_create(data, len, ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet) return 0;

    if (enet_peer_send(state->peer, 0, packet) < 0)
    {
        enet_packet_destroy(packet);
        return 0;
    }

    enet_host_flush(state->host);

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

    QMutexLocker locker(&state->mutex);

    if (!state->host) return 0;
    if (state->rxQueue.empty()) return 0;

    ENetPacket* packet = state->rxQueue.front();
    state->rxQueue.pop();

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
    QMutexLocker locker(&networkMutex);
    if (sock)
    {
        delete sock;
        sock = nullptr;
    }
    if (server)
    {
        delete server;
        server = nullptr;
    }
}

void IRSocketOpen(void* userdata)
{
    QMutexLocker locker(&networkMutex);

    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    bool isServer = cfg.GetBool("IR.Network.IsServer");
    int instanceID = inst->getInstanceID();
    if (instanceID > 0) isServer = false;

    if (isServer)
    {
        // SERVER MODE: Create server if needed
        if (!server)
        {
            server = new QTcpServer();
            int serverPort = cfg.GetInt("IR.Network.SelfPort");
            if (!server->listen(QHostAddress::Any, serverPort))
            {
                Log(LogLevel::Error, "Failed to start TCP server on port %d\n", serverPort);
                delete server;
                server = nullptr;
                return;
            }
            Log(LogLevel::Info, "TCP server listening on port %d\n", serverPort);
        }

        // Check for disconnected client
        if (sock && sock->state() != QAbstractSocket::ConnectedState)
        {
            delete sock;
            sock = nullptr;
            Log(LogLevel::Info, "Client disconnected\n");
        }

        // Accept new client if available
        if (!sock && server->hasPendingConnections())
        {
            QCoreApplication::processEvents();
            sock = server->nextPendingConnection();
            Log(LogLevel::Info, "Client connected from %s\n", sock->peerAddress().toString().toUtf8().constData());
        }
    }
    else
    {
        // CLIENT MODE: Connect to remote server
        if (!sock)
        {
            QString hostIP = cfg.GetQString("IR.Network.HostIP");
            int hostPort = cfg.GetInt("IR.Network.HostPort");

            sock = new QTcpSocket();
            sock->connectToHost(hostIP, hostPort);

            Log(LogLevel::Info, "TCP client connecting to %s:%d\n", hostIP.toUtf8().constData(), hostPort);

            if (sock->waitForConnected(10)) Log(LogLevel::Info, "Connected to %s:%d\n", hostIP.toUtf8().constData(), hostPort);

        }
        else if (sock->state() != QAbstractSocket::ConnectedState)
        {
            delete sock;
            sock = nullptr;
            Log(LogLevel::Info, "Disconnected from server\n");
        }
    }
}

u8 IRSendPacketTCP(char* data, int len, void* userdata)
{
    QCoreApplication::processEvents();
    IRSocketOpen(userdata);

    QMutexLocker locker(&networkMutex);

    if (!sock || sock->state() != QAbstractSocket::ConnectedState) return 0;

    qint64 bytesWritten = sock->write(data, len);
    sock->flush();

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

    QMutexLocker locker(&networkMutex);

    if (!sock || sock->bytesAvailable() <= 0) return 0;

    qint64 bytesRead = sock->read(data, len);

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
    if (!serial || !serial->isOpen()) return false;

    // Check for serial port errors that indicate disconnection
    QSerialPort::SerialPortError error = serial->error();
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
    if (serial) 
    {
        if (serial->isOpen()) serial->close();
        printf("Serial port closed\n");
        delete serial;
        serial = nullptr;
    }
}

void IRSerialOpenPort(void* userdata)
{
    if (!IRSerial()) IRSerialClosePort();

    if (!serial)
    {
        EmuInstance* inst = (EmuInstance*)userdata;
        auto& cfg = inst->getLocalConfig();
        QString portPath = cfg.GetQString("IR.SerialPortPath");
        serial = new QSerialPort();
        // Windows: COM ports above COM9 need \\.\COMxx format
        #ifdef _WIN32
        if (portPath.startsWith("COM", Qt::CaseInsensitive))
        {
            int portNum = portPath.mid(3).toInt();
            if (portNum > 9) portPath = "\\\\.\\" + portPath;
        }
        #endif

        serial->setPortName(portPath);
        Log(LogLevel::Info, "Attempting to open serial port: %s\n", portPath.toUtf8().constData());

        if (!serial->open(QIODevice::ReadWrite)) 
        {   
            Log(LogLevel::Error, "Failed to open serial port %1: %2\n", portPath.toUtf8().constData(), serial->errorString());
        }
        else {
            // Configure port settings AFTER opening
            serial->setBaudRate(QSerialPort::Baud115200);
            serial->setDataBits(QSerialPort::Data8);
            serial->setParity(QSerialPort::NoParity);
            serial->setStopBits(QSerialPort::OneStop);
            serial->setFlowControl(QSerialPort::NoFlowControl);

            // Explicitly set DTR and RTS high for device stability
            serial->setDataTerminalReady(true);
            // serial->setRequestToSend(true);
            serial->clear();
            Log(LogLevel::Info, "Serial port opened successfully: %1 (115200 8N1, DTR=1, RTS=0)\n", portPath.toUtf8().constData());
        }
    }
    
    return;
}

u8 IRSendPacketSerial(char* data, int len, void* userdata)
{
    QCoreApplication::processEvents();
    IRSerialOpenPort(userdata);

    if (!serial || !serial->isOpen()) 
    {
        Log(LogLevel::Error, "Serial write failed: port not open\n");
        return 0;
    }

    qint64 bytesWritten = serial->write(data, len);

    if (bytesWritten < 0) 
    {
        Log(LogLevel::Error, "Serial read error: %1\n", serial->errorString().toUtf8().constData());
        return 0;
    }

    serial->flush();

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


    if (!serial || !serial->isOpen() || !serial->bytesAvailable()) return 0;

    qint64 bytesRead = serial->read(data, len);

    if (bytesRead < 0) 
    {
        Log(LogLevel::Error, "Serial read error: %1\n", serial->errorString().toUtf8().constData());
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

}
