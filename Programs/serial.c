/*
 * BRLTTY - A background process providing access to the Linux console (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2004 by The BRLTTY Team. All rights reserved.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation.  Please see the file COPYING for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "serial.h"
#include "misc.h"

int
isSerialDevice (const char **path) {
  if (isQualifiedDevice(path, "serial")) return 1;
  return !isQualifiedDevice(path, NULL);
}

int
openSerialDevice (const char *path, int *descriptor, struct termios *attributes) {
  char *device;
  if ((device = getDevicePath(path))) {
    if ((*descriptor = open(device, O_RDWR|O_NOCTTY|O_NONBLOCK)) != -1) {
      if (isatty(*descriptor)) {
        if (setBlockingIo(*descriptor, 1)) {
          if (!attributes || (tcgetattr(*descriptor, attributes) != -1)) {
            LogPrint(LOG_DEBUG, "Serial device opened: %s: fd=%d", device, *descriptor);
            free(device);
            return 1;
          } else {
            LogPrint(LOG_ERR, "Cannot get attributes for '%s': %s", device, strerror(errno));
          }
        }
      } else {
        LogPrint(LOG_ERR, "Not a serial device: %s", device);
      }
      close(*descriptor);
      *descriptor = -1;
    } else {
      LogPrint(LOG_ERR, "Cannot open '%s': %s", device, strerror(errno));
    }
    free(device);
  }
  return 0;
}

void
rawSerialDevice (struct termios *attributes) {
  attributes->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  attributes->c_oflag &= ~OPOST;
  attributes->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  attributes->c_cflag &= ~(CSIZE | PARENB);
  attributes->c_cflag |= CS8;
}

int
flushSerialInput (int descriptor) {
  if (tcflush(descriptor, TCIFLUSH) != -1) return 1;
  LogError("TCIFLUSH");
  return 0;
}

int
flushSerialOutput (int descriptor) {
  if (tcflush(descriptor, TCOFLUSH) != -1) return 1;
  LogError("TCOFLUSH");
  return 0;
}

int
setSerialDevice (int descriptor, struct termios *attributes, speed_t baud) {
  if (cfsetispeed(attributes, baud) != -1) {
    if (cfsetospeed(attributes, baud) != -1) {
      if (tcsetattr(descriptor, TCSANOW, attributes) != -1) {
        return 1;
      } else {
        LogError("Serial device attributes set");
      }
    } else {
      LogError("Serial device output speed set");
    }
  } else {
    LogError("Serial device input speed set");
  }
  return 0;
}

int
resetSerialDevice (int descriptor, struct termios *attributes, speed_t baud) {
  if (flushSerialOutput(descriptor)) {
    if (setSerialDevice(descriptor, attributes, B0)) {
      delay(500);
      if (flushSerialInput(descriptor)) {
        if (setSerialDevice(descriptor, attributes, baud)) {
          return 1;
        }
      }
    }
  }
  return 0;
}

typedef struct {
  int integer;
  speed_t baud;
} BaudEntry;
static const BaudEntry baudTable[] = {
   #ifdef B50
   {     50, B50},
   #endif
   #ifdef B75
   {     75, B75},
   #endif
   #ifdef B110
   {    110, B110},
   #endif
   #ifdef B134
   {    134, B134},
   #endif
   #ifdef B150
   {    150, B150},
   #endif
   #ifdef B200
   {    200, B200},
   #endif
   #ifdef B300
   {    300, B300},
   #endif
   #ifdef B600
   {    600, B600},
   #endif
   #ifdef B1200
   {   1200, B1200},
   #endif
   #ifdef B1800
   {   1800, B1800},
   #endif
   #ifdef B2400
   {   2400, B2400},
   #endif
   #ifdef B4800
   {   4800, B4800},
   #endif
   #ifdef B9600
   {   9600, B9600},
   #endif
   #ifdef B19200
   {  19200, B19200},
   #endif
   #ifdef B38400
   {  38400, B38400},
   #endif
   #ifdef B57600
   {  57600, B57600},
   #endif
   #ifdef B115200
   { 115200, B115200},
   #endif
   #ifdef B230400
   { 230400, B230400},
   #endif
   #ifdef B460800
   { 460800, B460800},
   #endif
   #ifdef B500000
   { 500000, B500000},
   #endif
   #ifdef B576000
   { 576000, B576000},
   #endif
   #ifdef B921600
   { 921600, B921600},
   #endif
   #ifdef B1000000
   {1000000, B1000000},
   #endif
   #ifdef B1152000
   {1152000, B1152000},
   #endif
   #ifdef B1500000
   {1500000, B1500000},
   #endif
   #ifdef B2000000
   {2000000, B2000000},
   #endif
   #ifdef B2500000
   {2500000, B2500000},
   #endif
   #ifdef B3000000
   {3000000, B3000000},
   #endif
   #ifdef B3500000
   {3500000, B3500000},
   #endif
   #ifdef B4000000
   {4000000, B4000000},
   #endif
   {      0, B0}
};

int
validateBaud (speed_t *value, const char *description, const char *word, const unsigned int *choices) {
   int integer;
   if (!*word || isInteger(&integer, word)) {
      const BaudEntry *entry = baudTable;
      while (entry->integer) {
         if (integer == entry->integer) {
            if (choices) {
               while (*choices) {
                  if (integer == *choices) {
                     break;
                  }
                  ++choices;
               }
               if (!*choices) {
                  LogPrint(LOG_ERR, "Unsupported %s: %d",
                           description, integer);
                  return 0;
               }
            }
            *value = entry->baud;
            return 1;
         }
         ++entry;
      }
   }
   LogPrint(LOG_ERR, "Invalid %s: %d",
            description, integer);
   return 0;
}

int
baud2integer (speed_t baud) {
  const BaudEntry *entry = baudTable;
  while (entry->integer) {
    if (baud == entry->baud)
      return entry->integer;
    ++entry;
  }
  return -1;
}

void
initializeSerialAttributes (struct termios *attributes) {
  memset(attributes, 0, sizeof(*attributes));
  attributes->c_cflag = CREAD | CLOCAL | CS8;
  attributes->c_iflag = IGNPAR | IGNBRK;
}

int
setSerialDataBits (struct termios *attributes, int bits) {
  tcflag_t size;
  switch (bits) {
#ifdef CS5
    case 5: size = CS5; break;
#endif /* CS5 */

#ifdef CS6
    case 6: size = CS6; break;
#endif /* CS6 */

#ifdef CS7
    case 7: size = CS7; break;
#endif /* CS7 */

#ifdef CS8
    case 8: size = CS8; break;
#endif /* CS8 */

    default:
      LogPrint(LOG_WARNING, "Unknown serial data bit count: %d", bits);
      return 0;
  }

  attributes->c_cflag &= ~CSIZE;
  attributes->c_cflag |= size;
  return 1;
}

int
setSerialStopBits (struct termios *attributes, int bits) {
  if (bits == 1) {
    attributes->c_cflag &= ~CSTOPB;
  } else if (bits == 2) {
    attributes->c_cflag |= CSTOPB;
  } else {
    LogPrint(LOG_WARNING, "Unknown serial stop bit count: %d", bits);
    return 0;
  }
  return 1;
}

int
setSerialParity (struct termios *attributes, SerialParity parity) {
  if (parity == SERIAL_PARITY_NONE) {
    attributes->c_cflag &= ~(PARENB | PARODD);
  } else {
    if (parity == SERIAL_PARITY_EVEN) {
      attributes->c_cflag &= ~PARODD;
    } else if (parity == SERIAL_PARITY_ODD) {
      attributes->c_cflag |= PARODD;
    } else {
      LogPrint(LOG_WARNING, "Unknown serial parity: %c", parity);
      return 0;
    }
    attributes->c_cflag |= PARENB;
  }
  return 1;
}

int
setSerialFlowControl (struct termios *attributes, SerialFlowControl flow) {
#ifdef CRTSCTS
  if (flow & SERIAL_FLOW_HARDWARE) {
    attributes->c_cflag |= CRTSCTS;
    flow &= ~SERIAL_FLOW_HARDWARE;
  } else {
    attributes->c_cflag &= ~CRTSCTS;
  }
#else /* CRTSCTS */
#warning hardware flow control not settable on this platform
#endif /* CRTSCTS */

#ifdef IXOFF
  if (flow & SERIAL_FLOW_SOFTWARE_INPUT) {
    attributes->c_iflag |= IXOFF;
    flow &= ~SERIAL_FLOW_SOFTWARE_INPUT;
  } else {
    attributes->c_iflag &= ~IXOFF;
  }
#else /* IXOFF */
#warning software input flow control not settable on this platform
#endif /* IXOFF */

#ifdef IXON
  if (flow & SERIAL_FLOW_SOFTWARE_OUTPUT) {
    attributes->c_iflag |= IXON;
    flow &= ~SERIAL_FLOW_SOFTWARE_OUTPUT;
  } else {
    attributes->c_iflag &= ~IXON;
  }
#else /* IXON */
#warning software output flow control not settable on this platform
#endif /* IXON */

  if (flow) {
    LogPrint(LOG_WARNING, "Unknown serial flow control: 0X%02X", flow);
    return 0;
  }

  return 1;
}
