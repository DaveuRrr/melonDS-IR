#include <stdio.h>
#include <string.h>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMutex>
#include <QThread>
#include <QRandomGenerator>
#include <QtSerialPort/QSerialPort>

#include "Platform.h"
#include "Config.h"
#include "EmuInstance.h"


namespace melonDS::Platform
{

QSerialPort *serial = nullptr;
QTcpServer *server = nullptr;
QTcpSocket *sock = nullptr;
QMutex tcpMutex;

enum IRMode
{
    IR_PASSTHRU = 0,
    IR_SERIAL = 1,
    IR_TCP = 2,
};

/******************************************************************************
 * IR Socket TCP
******************************************************************************/
void IRSocketOpen(void * userdata)
{
    QMutexLocker locker(&tcpMutex);

    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    bool isServer = cfg.GetBool("IR.TCP.IsServer");

    if (isServer)
    {
        // SERVER MODE: Create server if needed
        if (!server)
        {
            server = new QTcpServer();
            int serverPort = cfg.GetInt("IR.TCP.SelfPort");
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
            QString hostIP = cfg.GetQString("IR.TCP.HostIP");
            int hostPort = cfg.GetInt("IR.TCP.HostPort");

            sock = new QTcpSocket();
            sock->connectToHost(hostIP, hostPort);

            Log(LogLevel::Info, "Attempting to connect to %s:%d\n", hostIP.toUtf8().constData(), hostPort);

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

u8 IRSendPacketTCP(char* data, int len, void * userdata)
{
    QCoreApplication::processEvents();
    IRSocketOpen(userdata);

    QMutexLocker locker(&tcpMutex);

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
        Log(LogLevel::Info, "Sent %lld bytes: %s\n", bytesWritten, stringBuffer);
    }

    return static_cast<u8>(bytesWritten);
}

u8 IRReceivePacketTCP(char* data, int len, void * userdata)
{
    QCoreApplication::processEvents();
    IRSocketOpen(userdata);

    QMutexLocker locker(&tcpMutex);

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
        Log(LogLevel::Info, "Received %lld bytes: %s\n", bytesRead, stringBuffer);
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

void IRSerialOpenPort(void * userdata)
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

u8 IRSendPacketSerial(char* data, int len, void * userdata)
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

u8 IRReceivePacketSerial(char* data, int len,void * userdata)
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
u8 IRSendPacket(char* data, int len, void * userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();

    int IRMode = cfg.GetInt("IR.Mode");
    if (IRMode != IR_SERIAL) IRSerialClosePort();

    //printf("Trying to send IR Packet in mode: %d\n", IRMode);
    switch(IRMode)
    {
        case IR_PASSTHRU: return IR_PASSTHRU;
        case IR_SERIAL: return IRSendPacketSerial(data, len, userdata);
        case IR_TCP: return IRSendPacketTCP(data, len, userdata);
        default: return IR_PASSTHRU;
    }
}

u8 IRReceivePacket(char* data, int len, void * userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();

    int IRMode = cfg.GetInt("IR.Mode");
    if (IRMode != IR_SERIAL) IRSerialClosePort();

    //printf("Trying to recieve IR Packet in mode: %d\n", IRMode);
    switch(IRMode)
    {
        case IR_PASSTHRU: return IR_PASSTHRU;
        case IR_SERIAL: return IRReceivePacketSerial(data, len, userdata);
        case IR_TCP: return IRReceivePacketTCP(data, len, userdata);
        default: return IR_PASSTHRU;
    }
}

}
