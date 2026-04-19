#ifndef ANDROIDIRHANDLER_H
#define ANDROIDIRHANDLER_H

namespace MelonDSAndroid
{
    class AndroidIRHandler {
    public:
        virtual bool openSerial() = 0;
        virtual void closeSerial() = 0;
        virtual int writeSerial(const char* data, int length) = 0;
        virtual int readSerial(char* buffer, int maxLength) = 0;
        virtual int readSerialBlocking(char* buffer, int maxLength, long long timeoutMs) = 0;
        virtual bool isSerialOpen() = 0;
        virtual bool openTCP() = 0;
        virtual void closeTCP() = 0;
        virtual int writeTCP(const char* data, int length) = 0;
        virtual int readTCP(char* buffer, int maxLength) = 0;
        virtual bool isTCPOpen() = 0;
        virtual bool hasDataAvailable() = 0;
        virtual ~AndroidIRHandler() {};
    };
}

#endif //ANDROIDIRHANDLER_H
