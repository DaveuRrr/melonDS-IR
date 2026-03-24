#include <stdio.h>
#include <string.h>

#ifdef __WIN32__
    #include <winsock2.h>
    #include <ws2tcpip.h>

    #define socket_t    SOCKET
    #define sockaddr_t  SOCKADDR
    #define sockaddr_in_t  SOCKADDR_IN
#else
    #include <unistd.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>

    #define socket_t    int
    #define sockaddr_t  struct sockaddr
    #define sockaddr_in_t  struct sockaddr_in
    #define closesocket close
#endif

#ifndef INVALID_SOCKET
    #define INVALID_SOCKET  (socket_t)-1
#endif

// #include <QTcpServer>
// #include <QTcpSocket>
// #include <QDateTime>


#include <QtSerialPort/QSerialPort>

#include "Platform.h"
#include "Config.h"
#include "EmuInstance.h"


namespace melonDS::Platform
{

QSerialPort *serial = nullptr;
// QTcpServer *server = nullptr;
// QTcpSocket *sock = nullptr;
socket_t server = INVALID_SOCKET;
socket_t sock = INVALID_SOCKET;

enum IRMode
{
    IR_PASSTHRU = 0,
    IR_SERIAL = 1,
    IR_TCP = 2,
};

/******************************************************************************
 * IR Socket TCP
******************************************************************************/
bool IRSocketOpen(void * userdata)
{
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    bool isServer = cfg.GetBool("IR.TCP.IsServer");

    if (isServer) 
    {
        // SERVER MODE: Listen for incoming connections
        if (server == INVALID_SOCKET)
        {
            server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (server == INVALID_SOCKET)
            {
                Log(LogLevel::Error, "IRSocketOpen: Failed to create server socket\n");
                return false;
            }

            int serverPort = cfg.GetInt("IR.TCP.SelfPort");
            sockaddr_in_t serverAddr;
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_addr.s_addr = INADDR_ANY;
            serverAddr.sin_port = htons(serverPort);

            if (bind(server, (sockaddr_t*)&serverAddr, sizeof(serverAddr)) < 0)
            {
                Log(LogLevel::Error, "IRSocketOpen: Failed to bind server socket\n");
                return false;
            }
            
            if (listen(server, 1) < 0)
            {
                Log(LogLevel::Error, "IRSocketOpen: Failed to listen on server socket\n");
                closesocket(server);
                return false;
            }
            Log(LogLevel::Info, "IRSocketOpen: TCP server listening on port %d\n", serverPort);
        }

        // SERVER MODE: Listening to client
        if (sock != INVALID_SOCKET)
        {
            char buf[1];
            int result = recv(sock, buf, 1, MSG_PEEK);
            if (result == 0 || (result < 0 && 
#ifdef __WIN32__
                WSAGetLastError() != WSAEWOULDBLOCK
#else
                errno != EWOULDBLOCK && errno != EAGAIN
#endif
            ))
            {
                closesocket(sock);
                sock = INVALID_SOCKET;
                Log(LogLevel::Info, "Client disconnected\n");
            }
        }

        if (sock == INVALID_SOCKET)
        {
            fd_set readFDS;
            FD_ZERO(&readFDS);
            FD_SET(server, &readFDS);
            struct timeval tv = {0, 0};
            if (select(server + 1, &readFDS, NULL, NULL, &tv) > 0)
            {
                sockaddr_in_t clientAddr;
                socklen_t addrLen = sizeof(clientAddr);
                sock = accept(server, (sockaddr_t*)&clientAddr, &addrLen);
                if (sock != INVALID_SOCKET) Log(LogLevel::Info, "Client connected");
            }
        }

    } 
    else 
    {
        // CLIENT MODE: Connect to remote server
        if (sock == INVALID_SOCKET)
        {
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET)
            {
                Log(LogLevel::Error, "IRSocketOpen: Failed to create client socket\n");
                return false;
            }

            QString hostIP = cfg.GetQString("IR.TCP.HostIP");
            int hostPort = cfg.GetInt("IR.TCP.HostPort");
            sockaddr_in_t serverAddr;
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(hostPort);

#ifdef __WIN32__
            if (InetPtonA(AF_INET, hostIP.toUtf8().constData(), &serverAddr.sin_addr) <= 0) 
#else
            if (inet_aton(hostIP.toUtf8().constData(), &serverAddr.sin_addr) == 0) 
#endif
            {
                closesocket(sock);
                sock = INVALID_SOCKET;
                Log(LogLevel::Error, "IRSocketOpen: Invalid IP address %s\n", hostIP.toUtf8().constData());
                return false;
            }

            Log(LogLevel::Info, "Attempting to connect to %s:%d\n", hostIP.toUtf8().constData(), hostPort);

            if (connect(sock, (sockaddr_t*)&serverAddr, sizeof(serverAddr)) < 0) 
            {
                Log(LogLevel::Error, "Connection failed\n");
                closesocket(sock);
                sock = INVALID_SOCKET;
                return false;
            }

            Log(LogLevel::Info, "Connected to server %s:%d\n", hostIP.toUtf8().constData(), hostPort);
        }
        else
        {

            char buf[1];
            int result = recv(sock, buf, 1, MSG_PEEK);
            if (result == 0 || (result < 0 && 
#ifdef __WIN32__
                WSAGetLastError() != WSAEWOULDBLOCK
#else
                errno != EWOULDBLOCK && errno != EAGAIN
#endif
            ))
            {
                closesocket(sock);
                sock = INVALID_SOCKET;
                Log(LogLevel::Info, "Disconnected from server\n");
            }
        }
    }

    return (sock != INVALID_SOCKET);
}

u8 IRSendPacketTCP(char* data, int len, void * userdata)
{
    if (!IRSocketOpen(userdata))
    {
        Log(LogLevel::Error, "No connection to send IR data\n");
        return 0;
    }

    int bytesWritten = send(sock, data, len, 0);
    if (bytesWritten > 0) 
    {
        Log(LogLevel::Info, "Sent %d bytes\n", bytesWritten);
        return static_cast<int>(bytesWritten);
    }

    return 0;
}

u8 IRReceivePacketTCP(char* data, int len, void * userdata)
{
    if (!IRSocketOpen(userdata))
    {
        Log(LogLevel::Error, "No connection to receive IR data\n");
        return 0;
    }

    int bytesRead = recv(sock, data, len, 0);
    if (bytesRead > 0) 
    {
        Log(LogLevel::Info, "Received %d bytes\n", bytesRead);
        return static_cast<int>(bytesRead);
    }

    return 0;
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
