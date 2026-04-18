#ifndef MELONDS_ANDROID_IR_H
#define MELONDS_ANDROID_IR_H

#include <cstdint>

using u8 = uint8_t;

// Forward declaration
namespace MelonDSAndroid {
    class AndroidIRHandler;
}

namespace melonDS::Platform {
    extern int irMode; // 0 = None (disabled), 1 = USB Serial, 2 = TCP, 3 = Direct Storage

    /**
     * Set the IR handler instance
     * @param handler Pointer to the platform-specific IR handler
     */
    void setHandler(MelonDSAndroid::AndroidIRHandler* handler);

    /**
     * Set the IR transport mode
     * @param mode 0 = None (disabled), 1 = USB Serial, 2 = TCP, 3 = Direct Storage
     */
    void setIRMode(int mode);
}

#endif // MELONDS_ANDROID_IR_H
