
#include <QTcpServer>
#include <QTcpSocket>
#include <QtSerialPort/QSerialPort>
#include <QDateTime>



#include "Platform.h"
#include "Config.h"
#include "EmuInstance.h"

#include "PokeEscalator.h"
//Improvements to be made.

namespace melonDS::Platform
{



//---------------------TCP = 2
QTcpServer *server = nullptr;
QTcpSocket *sock = nullptr;
void IR_OpenTCP(void * userdata){
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    bool isServer = cfg.GetBool("IR.TCP.IsServer");
    if (isServer) {
        // SERVER MODE: Listen for incoming connections
        if (!server){
            int port = cfg.GetInt("IR.TCP.SelfPort");
            server = new QTcpServer();
            if (!server->listen(QHostAddress::Any, port)) {
                printf("Failed to start TCP server on port %d: %s\n", port, server->errorString().toUtf8().constData());
                return;
            }
            printf("TCP server listening on port %d\n", port);
        }

        // Clean up disconnected socket
        if (sock && sock->state() != QAbstractSocket::ConnectedState) {
            delete sock;
            sock = nullptr;
            printf("Client disconnected\n");
        }

        // Check for new connection (non-blocking)
        if (!sock) {
            QCoreApplication::processEvents();
            if (server->hasPendingConnections()) {
                sock = server->nextPendingConnection();
                printf("Client connected from %s\n", sock->peerAddress().toString().toUtf8().constData());
            }
        }
    } else {
        // CLIENT MODE: Connect to remote server
        if (!sock) {
            QString hostIP = cfg.GetQString("IR.TCP.HostIP");
            int hostPort = cfg.GetInt("IR.TCP.HostPort");

            sock = new QTcpSocket();
            printf("Attempting to connect to %s:%d\n", hostIP.toUtf8().constData(), hostPort);
            sock->connectToHost(hostIP, hostPort);

            // Wait briefly for connection
            if (sock->waitForConnected(10)) {
                printf("Connected to server %s:%d\n", hostIP.toUtf8().constData(), hostPort);
            } else {
                printf("Connection failed: %s\n", sock->errorString().toUtf8().constData());
                delete sock;
                sock = nullptr;
            }
        }
        // Clean up disconnected socket
        else if (sock->state() != QAbstractSocket::ConnectedState) {
            delete sock;
            sock = nullptr;
            printf("Disconnected from server\n");
        }
    }
}

u8 IR_TCP_SendPacket(char* data, int len, void * userdata){
    IR_OpenTCP(userdata);
    QCoreApplication::processEvents();

    if (!sock || sock->state() != QAbstractSocket::ConnectedState){
        printf("No client connected to send IR data\n");
        return 0;
    }

    qint64 written = sock->write(data, len);
    sock->flush();

    printf("Sent %lld bytes to client\n", written);
    return 0;
}


u8 IR_TCP_RecievePacket(char* data, int len, void * userdata){
    IR_OpenTCP(userdata);
    QCoreApplication::processEvents();
    if (!sock || sock->bytesAvailable() <= 0){
        return 0;
    }
    qint64 bytesRead = sock->read(data, len); //len is maxLen

    if (bytesRead > 0){
        return static_cast<int>(bytesRead);
    }
    return 0;
}



//--------------------SERIAL = 1
QSerialPort *serial = nullptr;

bool IR_IsSerialHealthy(){
    if (!serial || !serial->isOpen()) {
        return false;
    }

    // Check for serial port errors that indicate disconnection
    QSerialPort::SerialPortError error = serial->error();
    if (error == QSerialPort::ResourceError ||
        error == QSerialPort::PermissionError ||
        error == QSerialPort::DeviceNotFoundError) {
        return false;
    }

    return true;
}

void IR_CloseSerialPort(){
    if (serial) {
        if (serial->isOpen()) {
            serial->close();
            FILE* log = fopen("ir_serial.log", "a");
            if (log) {
                fprintf(log, "Serial port closed\n");
                fflush(log);
                fclose(log);
            }
            printf("Serial port closed\n");
        }
        delete serial;
        serial = nullptr;
    }
}

void IR_OpenSerialPort(void * userdata){
    if (!IR_IsSerialHealthy()) {
        IR_CloseSerialPort();
    }

    if (!serial){

        EmuInstance* inst = (EmuInstance*)userdata;
        auto& cfg = inst->getLocalConfig();

        serial = new QSerialPort();

        QString portPath = cfg.GetQString("IR.SerialPortPath");

        // Windows: COM ports above COM9 need \\.\COMxx format
        #ifdef _WIN32
        if (portPath.startsWith("COM", Qt::CaseInsensitive)) {
            int portNum = portPath.mid(3).toInt();
            if (portNum > 9) {
                portPath = "\\\\.\\" + portPath;
            }
        }
        #endif

        serial->setPortName(portPath);

        // Log to file for debugging
        FILE* log = fopen("ir_serial.log", "a");
        if (log) {
            fprintf(log, "Attempting to open serial port: %s\n", portPath.toUtf8().constData());
            fflush(log);
        }

        if (!serial->open(QIODevice::ReadWrite)) {
            QString error = QString("Failed to open serial port %1: %2\n").arg(portPath).arg(serial->errorString());
            printf("%s", error.toUtf8().constData());
            if (log) {
                fprintf(log, "%s", error.toUtf8().constData());
                fflush(log);
            }
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

            // Clear any stale data in buffers
            serial->clear();

            QString success = QString("Serial port opened successfully: %1 (115200 8N1, DTR=1, RTS=1)\n").arg(portPath);
            printf("%s", success.toUtf8().constData());
            if (log) {
                fprintf(log, "%s", success.toUtf8().constData());
                fflush(log);
            }
        }

        if (log) fclose(log);
    }
    else return;
}
u8 IR_Serial_SendPacket(char* data, int len, void * userdata){
    IR_OpenSerialPort(userdata);
    QCoreApplication::processEvents(); // allow Qt to update I/O status

    FILE* log = fopen("ir_serial.log", "a");

    if (!serial || !serial->isOpen()) {
        printf("Serial write failed: port not open\n");
        if (log) {
            fprintf(log, "Serial write failed: port not open\n");
            fclose(log);
        }
        return 0;
    }

    qint64 written = serial->write(data, len);

    if (written < 0) {
        QString error = QString("Serial write error: %1\n").arg(serial->errorString());
        printf("%s", error.toUtf8().constData());
        if (log) {
            fprintf(log, "%s", error.toUtf8().constData());
            fclose(log);
        }
        return 0;
    }

    serial->flush();

    printf("Serial wrote %lld bytes: ", written);
    for (int i = 0; i < len; ++i)
        printf("%02X ", static_cast<unsigned char>(data[i]));
    printf("\n");

    if (log) {
        fprintf(log, "Serial wrote %lld bytes: ", written);
        for (int i = 0; i < len; ++i)
            fprintf(log, "%02X ", static_cast<unsigned char>(data[i]));
        fprintf(log, "\n");
        fflush(log);
        fclose(log);
    }

    return static_cast<u8>(written);
}
u8 IR_Serial_RecievePacket(char* data, int len,void * userdata){
    IR_OpenSerialPort(userdata);
    QCoreApplication::processEvents(); // allow Qt to update I/O status

    if (!serial || !serial->isOpen() || !serial->bytesAvailable()) {
        return 0;
    }

    FILE* log = fopen("ir_serial.log", "a");
    qint64 bytesRead = serial->read(data, len);

    if (bytesRead < 0) {
        QString error = QString("Serial read error: %1\n").arg(serial->errorString());
        printf("%s", error.toUtf8().constData());
        if (log) {
            fprintf(log, "%s", error.toUtf8().constData());
            fclose(log);
        }
        return 0;
    }

    if (bytesRead > 0) {
        printf("Serial Read %lld bytes: ", bytesRead);
        for (int i = 0; i < bytesRead; ++i)
            printf("%02X ", static_cast<unsigned char>(data[i]));
        printf("\n");

        if (log) {
            fprintf(log, "Serial Read %lld bytes: ", bytesRead);
            for (int i = 0; i < bytesRead; ++i)
                fprintf(log, "%02X ", static_cast<unsigned char>(data[i]));
            fprintf(log, "\n");
            fflush(log);
            fclose(log);
        }

        return static_cast<u8>(bytesRead);
    }

    if (log) fclose(log);
    return 0;
}






/*
   -=+Direct Mode+=-

   We have a bit of heavy lifting to do here. Mostly because the game does not really expect instant replies. For example, the game seems to issue one read (0x01) before two version checks (0x08). If this
   read is populated with any data, it will brick the rest of the comms session. Usually this is not a problem because it would be incredibly hard to do in real life, and its possible that the internal
   IR chip prevents this anyways. So here, we use a combination of the last packet recieve time and an 'init' sequence counter to establish when a "new" session is created. This helps us deal with file handling
   as well.
        - I could have maybe rewrote things to account for the version check in here, but that seemed annoying. (This IR implementation will never see 0x08 issued, or anything except 0x01 and 0x02).

    Other than that, this mode just heavily relies on the pwalker implementation. Because we are the 'master' here but do not advertise, the init sequence with the pwalker emulator is a bit of a hack, but not
    very complex or annoying.

*/

u64 lastPacketTime = 0;
int init = 0;
FileHandle * eh = nullptr;




/*
    Because we will work on a send -> immediately recieve basis, we store the response to a Tx in the 'directRxBuffer'. This lets us easily read later when the game actually issues an 'IR Recieve'
    This greatly simplifies the work that needs to be done by the walker emulator implementaiton.

    'wait' exists to ensure compliance with timing specs. When 'waiting', the walker is sending NO DATA, allowing the game to recieve NO DATA for the minimum time (3.5ms).
    We clear this when initing coms, or we have Txed and expect real data.

    This is done to make NDSCart.cpp not have to check for direct mode, and essentially just enforces direct mode to comply with real timing specs.
*/
char directRxBuffer[0xb8];
int16_t directRxLength;
bool wait = 0; //Set this to wait for next Tx

/*
    Send the walker emulator a packet and store the response

*/
u8 IR_Direct_SendPacket(char* data, int len, void * userdata){
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();
    directRxLength = txToWalker((FILE *) eh, len, data, directRxBuffer);
    wait = false;
    return 0; //Txing return is unused currently.
}




/*
    The game starts by Rxing for an advertisement, and must do some things to establish a 'new connection' so we can properly handle the file.
    Return: num of bytes recieved
*/
u8 IR_Direct_RecievePacket(char* data, int len,void * userdata){
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();


    /*
        Establish a new connection if we have no activity for a certain period of time.
        Reset init sequence to 0. (Init sequence is essentially # of Rx Packets to ignore before we start comms)
    */
    if ((Platform::GetMSCount() - lastPacketTime) > 1000){
        init = 0;
        wait = 0;
        lastPacketTime = Platform::GetMSCount();
        eh = nullptr; //We expect the pwalker implementation to close this file (Currently anyways). However we need to invalidate that file pointer or else we will crash when operating on it again.
        return 0;
    }


    // All data has been sent, send 0 bytes to ensure the game can wait for timing.
    if (wait) return 0;


    // 10 = ignoreCount. probably can be 1 or 2 but doesnt really change much for the user.
    if (init < 10){
        init++;
        return 0;
    }
    else if (init == 10){

        init++;
        printf("Starting walker IR sequence\n");
        if (!eh) eh = OpenFile(cfg.GetString("IR.EEPROMPath"), FileMode::ReadWriteExisting);

        wait = true;
        //This is the first communication, so we need to 'stimulate' the walker sending an advertisement. rtnlen SHOULD be 1 here and the data SHOULD be the advertisement packet. (0xfc)
        return txToWalker((FILE *) eh, 0, data, data);
    }


    /*
        This is now the 'normal' communication. Keep in mind that the 'directRxLength' and 'directRxBuffer' will be properly manipulated by directTx.
        Populate the correct buffer with the info
    */
    lastPacketTime = Platform::GetMSCount();
    for (int i = 0; i < directRxLength; i++) data[i] = (char) directRxBuffer[i];
    wait = true;
    return directRxLength;

}







//--------------------------------Global
static int lastIRMode = -1; // Track previous mode

u8 IR_SendPacket(char* data, int len, void * userdata){
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();

    int IRMode = cfg.GetInt("IR.Mode");

    // Cleanup when mode changes
    if (IRMode != lastIRMode) {
        if (lastIRMode == 1) {
            IR_CloseSerialPort();
        }
    }
    lastIRMode = IRMode;

    //printf("Trying to send IR Packet in mode: %d\n", IRMode);

    if (IRMode == 0) return 0;
    if (IRMode == 1) return IR_Serial_SendPacket(data, len, userdata);
    if (IRMode == 2) return IR_TCP_SendPacket(data, len, userdata);
    if (IRMode == 3) return IR_Direct_SendPacket(data,len,userdata);
    return 0;
}
u8 IR_RecievePacket(char* data, int len, void * userdata){
    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();

    int IRMode = cfg.GetInt("IR.Mode");

    // Cleanup when mode changes
    if (IRMode != lastIRMode) {
        if (lastIRMode == 1) {
            IR_CloseSerialPort();
        }
    }
    lastIRMode = IRMode;

    //printf("Trying to recieve IR Packet in mode: %d\n", IRMode);

    if (IRMode == 0) return 0;
    if (IRMode == 1) return IR_Serial_RecievePacket(data, len, userdata);
    if (IRMode == 2) return IR_TCP_RecievePacket(data, len, userdata);
    if (IRMode == 3) return IR_Direct_RecievePacket(data, len, userdata);
    return 0;
}


FileHandle * fh = nullptr;
//= OpenFile(cfg.GetString("IR.PacketLogFile"), FileMode::Append);
void IR_LogPacket(char * data, int len, bool isTx, void * userdata){


    char key = 0xAA;
    //printf("Trying to log a packet. Len: %d IsTx: %d \n", len, isTx);

    EmuInstance* inst = (EmuInstance*)userdata;
    auto& cfg = inst->getLocalConfig();

    std::string logPath = cfg.GetString("IR.PacketLogFile");
    if (logPath.empty()) return;
    if (!fh) fh = OpenFile(logPath, FileMode::Append);
    // if (!fh) fh = OpenFile(cfg.GetString("IR.PacketLogFile"), FileMode::Append);


    if (isTx) FileWrite("Tx: ", 1, 4, fh);
    else FileWrite("Rx: ", 1, 4, fh);


    for (size_t i = 0; i < len; i++){
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", static_cast<unsigned char>(data[i] ^ key));
        FileWrite(hex, 1, strlen(hex), fh);
    }

    FileWrite("\n", 1, 1, fh);
    fflush((FILE *)fh);

}


}
