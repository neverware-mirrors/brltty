/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2013 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version. Please see the file LICENSE-GPL for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <string.h>
#include <errno.h>

#ifdef __MINGW32__
#if WINVER < WindowsVista
#define CancelIoEx(handle, ol) CancelIo((handle))
#endif /* WINVER < WindowsVista */
#endif /* __MINGW32__ */

#ifdef __MSDOS__
#include "system_msdos.h"
#endif /* __MSDOS__ */

#undef ASYNC_CAN_MONITOR_IO
#if defined(__MINGW32__)
#define ASYNC_CAN_MONITOR_IO

typedef HANDLE MonitorEntry;

#elif defined(HAVE_SYS_POLL_H)
#define ASYNC_CAN_MONITOR_IO

#include <sys/poll.h>
typedef struct pollfd MonitorEntry;

#elif defined(HAVE_SELECT)
#define ASYNC_CAN_MONITOR_IO

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#else /* HAVE_SYS_SELECT_H */
#include <sys/time.h>
#endif /* HAVE_SYS_SELECT_H */

typedef struct {
  int size;
  fd_set set;
} SelectDescriptor;

static SelectDescriptor selectDescriptor_read;
static SelectDescriptor selectDescriptor_write;
static SelectDescriptor selectDescriptor_exception;

typedef struct {
  fd_set *selectSet;
  FileDescriptor fileDescriptor;
} MonitorEntry;

#endif /* monitor definitions */

#include "log.h"
#include "async_io.h"
#include "async_internal.h"
#include "timing.h"

typedef struct FunctionEntryStruct FunctionEntry;

typedef struct {
  AsyncMonitorCallback *callback;
} MonitorExtension;

typedef union {
  struct {
    AsyncInputCallback *callback;
    unsigned end:1;
  } input;

  struct {
    AsyncOutputCallback *callback;
  } output;
} TransferDirectionUnion;

typedef struct {
  TransferDirectionUnion direction;
  size_t size;
  size_t length;
  unsigned char buffer[];
} TransferExtension;

typedef struct {
  FunctionEntry *function;
  void *extension;
  void *data;

  MonitorEntry *monitor;
  int error;

  unsigned active:1;
  unsigned cancel:1;
  unsigned finished:1;
} OperationEntry;

typedef struct {
  const char *functionName;

  void (*beginFunction) (FunctionEntry *function);
  void (*endFunction) (FunctionEntry *function);

  void (*startOperation) (OperationEntry *operation);
  void (*finishOperation) (OperationEntry *operation);
  void (*cancelOperation) (OperationEntry *operation);

  int (*invokeCallback) (OperationEntry *operation);
} FunctionMethods;

struct FunctionEntryStruct {
  FileDescriptor fileDescriptor;
  const FunctionMethods *methods;
  Queue *operations;

#if defined(__MINGW32__)
  struct {
    OVERLAPPED overlapped;
  } windows;

#elif defined(HAVE_SYS_POLL_H)
  struct {
    short int events;
  } poll;

#elif defined(HAVE_SELECT)
  struct {
    SelectDescriptor *descriptor;
  } select;

#endif /* monitor paradigms */
};

typedef struct {
  FileDescriptor fileDescriptor;
  const FunctionMethods *methods;
} FunctionKey;

typedef struct {
  MonitorEntry *const array;
  unsigned int count;
} MonitorGroup;

#ifdef __MINGW32__
static void
prepareMonitors (void) {
}

static int
awaitMonitors (const MonitorGroup *monitors, int timeout) {
  if (monitors->count) {
    DWORD result = WaitForMultipleObjects(monitors->count, monitors->array, FALSE, timeout);
    if ((result >= WAIT_OBJECT_0) && (result < (WAIT_OBJECT_0 + monitors->count))) return 1;

    if (result == WAIT_FAILED) {
      logWindowsSystemError("WaitForMultipleObjects");
    }
  } else {
    approximateDelay(timeout);
  }

  return 0;
}

static void
initializeMonitor (MonitorEntry *monitor, const FunctionEntry *function, const OperationEntry *operation) {
  *monitor = function->windows.overlapped.hEvent;
  if (*monitor == INVALID_HANDLE_VALUE) *monitor = function->fileDescriptor;
}

static int
testMonitor (const MonitorEntry *monitor, const FunctionEntry *function) {
  DWORD result = WaitForSingleObject(*monitor, 0);
  if (result == WAIT_OBJECT_0) return 1;

  if (result == WAIT_FAILED) {
    logWindowsSystemError("WaitForSingleObject");
  }

  return 0;
}

static int
allocateWindowsEvent (HANDLE *event) {
  if (*event == INVALID_HANDLE_VALUE) {
    HANDLE handle = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!handle) return 0;
    *event = handle;
  }

  return ResetEvent(*event);
}

static void
deallocateWindowsEvent (HANDLE *event) {
  if (*event != INVALID_HANDLE_VALUE) {
    CloseHandle(*event);
    *event = INVALID_HANDLE_VALUE;
  }
}

static int
allocateWindowsResources (OperationEntry *operation) {
  FunctionEntry *function = operation->function;

  if (allocateWindowsEvent(&function->windows.overlapped.hEvent)) {
    return 1;
  }

  operation->finished = 1;
  operation->error = GetLastError();
  return 0;
}

static void
setWindowsTransferResult (OperationEntry *operation, DWORD success, DWORD count) {
  TransferExtension *extension = operation->extension;

  if (success) {
    extension->length += count;
  } else {
    DWORD error = GetLastError();

    if ((error == ERROR_HANDLE_EOF) || (error == ERROR_BROKEN_PIPE)) {
      extension->direction.input.end = 1;
    } else {
      setErrno(error);
      operation->error = errno;

      if (error == ERROR_IO_PENDING) return;
      if (error == ERROR_IO_INCOMPLETE) return;
    }
  }

  operation->finished = 1;
}

static void
beginWindowsFunction (FunctionEntry *function) {
  ZeroMemory(&function->windows.overlapped, sizeof(function->windows.overlapped));
  function->windows.overlapped.hEvent = INVALID_HANDLE_VALUE;
}

static void
endWindowsFunction (FunctionEntry *function) {
  deallocateWindowsEvent(&function->windows.overlapped.hEvent);
}

static void
startWindowsRead (OperationEntry *operation) {
  FunctionEntry *function = operation->function;
  TransferExtension *extension = operation->extension;

  if (allocateWindowsResources(operation)) {
    DWORD count;
    BOOL success = ReadFile(function->fileDescriptor,
                            &extension->buffer[extension->length],
                            extension->size - extension->length,
                            &count, &function->windows.overlapped);

    setWindowsTransferResult(operation, success, count);
  }
}

static void
startWindowsWrite (OperationEntry *operation) {
  FunctionEntry *function = operation->function;
  TransferExtension *extension = operation->extension;

  if (allocateWindowsResources(operation)) {
    DWORD count;
    BOOL success = WriteFile(function->fileDescriptor,
                             &extension->buffer[extension->length],
                             extension->size - extension->length,
                             &count, &function->windows.overlapped);

    setWindowsTransferResult(operation, success, count);
  }
}

static void
finishWindowsTransferOperation (OperationEntry *operation) {
  FunctionEntry *function = operation->function;
  DWORD count;
  BOOL success = GetOverlappedResult(function->fileDescriptor, &function->windows.overlapped, &count, FALSE);

  setWindowsTransferResult(operation, success, count);
}

static void
cancelWindowsTransferOperation (OperationEntry *operation) {
  FunctionEntry *function = operation->function;
  DWORD count;

  if (CancelIoEx(function->fileDescriptor, &function->windows.overlapped)) {
    GetOverlappedResult(function->fileDescriptor, &function->windows.overlapped, &count, TRUE);
  }
}

#else /* __MINGW32__ */

#ifdef HAVE_SYS_POLL_H
static void
prepareMonitors (void) {
}

static int
awaitMonitors (const MonitorGroup *monitors, int timeout) {
  int result = poll(monitors->array, monitors->count, timeout);
  if (result > 0) return 1;

  if (result == -1) {
    if (errno != EINTR) logSystemError("poll");
  }

  return 0;
}

static void
initializeMonitor (MonitorEntry *monitor, const FunctionEntry *function, const OperationEntry *operation) {
  monitor->fd = function->fileDescriptor;
  monitor->events = function->poll.events;
  monitor->revents = 0;
}

static int
testMonitor (const MonitorEntry *monitor, const FunctionEntry *function) {
  return (monitor->revents & function->poll.events) != 0;
}

static void
beginUnixInputFunction (FunctionEntry *function) {
  function->poll.events = POLLIN;
}

static void
beginUnixOutputFunction (FunctionEntry *function) {
  function->poll.events = POLLOUT;
}

static void
beginUnixAlertFunction (FunctionEntry *function) {
  function->poll.events = POLLPRI;
}

#elif defined(HAVE_SELECT)

static void
prepareSelectDescriptor (SelectDescriptor *descriptor) {
  FD_ZERO(&descriptor->set);
  descriptor->size = 0;
}

static void
prepareMonitors (void) {
  prepareSelectDescriptor(&selectDescriptor_read);
  prepareSelectDescriptor(&selectDescriptor_write);
  prepareSelectDescriptor(&selectDescriptor_exception);
}

static fd_set *
getSelectSet (SelectDescriptor *descriptor) {
  return descriptor->size? &descriptor->set: NULL;
}

static int
doSelect (int setSize, fd_set *readSet, fd_set *writeSet, fd_set *exceptionSet, int timeout) {
  struct timeval time = {
    .tv_sec = timeout / MSECS_PER_SEC,
    .tv_usec = (timeout % MSECS_PER_SEC) * USECS_PER_MSEC
  };

  {
    int result = select(setSize, readSet, writeSet, exceptionSet, &time);
    if (result > 0) return 1;

    if (result == -1) {
      if (errno != EINTR) logSystemError("select");
    }

    return 0;
  }
}

static int
awaitMonitors (const MonitorGroup *monitors, int timeout) {
  fd_set *readSet = getSelectSet(&selectDescriptor_read);
  fd_set *writeSet = getSelectSet(&selectDescriptor_write);
  fd_set *exceptionSet = getSelectSet(&selectDescriptor_exception);

  int setSize = selectDescriptor_read.size;
  setSize = MAX(setSize, selectDescriptor_write.size);
  setSize = MAX(setSize, selectDescriptor_exception.size);

#ifdef __MSDOS__
  int elapsed = 0;

  do {
    fd_set readSet1, writeSet1, exceptionSet1;

    if (readSet) readSet1 = *readSet;
    if (writeSet) writeSet1 = *writeSet;
    if (exceptionSet) exceptionSet1 = *exceptionSet;

    if (doSelect(setSize,
                 (readSet? &readSet1: NULL),
                 (writeSet? &writeSet1: NULL),
                 (exceptionSet? &exceptionSet1: NULL),
                 0)) {
      if (readSet) *readSet = readSet1;
      if (writeSet) *writeSet = writeSet1;
      if (exceptionSet) *exceptionSet = exceptionSet1;
      return 1;
    }
  } while ((elapsed += tsr_usleep(USECS_PER_MSEC)) < timeout);
#else /* __MSDOS__ */
  if (doSelect(setSize, readSet, writeSet, exceptionSet, timeout)) return 1;
#endif /* __MSDOS__ */

  return 0;
}

static void
initializeMonitor (MonitorEntry *monitor, const FunctionEntry *function, const OperationEntry *operation) {
  monitor->selectSet = &function->select.descriptor->set;
  monitor->fileDescriptor = function->fileDescriptor;
  FD_SET(function->fileDescriptor, &function->select.descriptor->set);

  if (function->fileDescriptor >= function->select.descriptor->size) {
    function->select.descriptor->size = function->fileDescriptor + 1;
  }
}

static int
testMonitor (const MonitorEntry *monitor, const FunctionEntry *function) {
  return FD_ISSET(monitor->fileDescriptor, monitor->selectSet);
}

static void
beginUnixInputFunction (FunctionEntry *function) {
  function->select.descriptor = &selectDescriptor_read;
}

static void
beginUnixOutputFunction (FunctionEntry *function) {
  function->select.descriptor = &selectDescriptor_write;
}

static void
beginUnixAlertFunction (FunctionEntry *function) {
  function->select.descriptor = &selectDescriptor_exception;
}

#endif /* Unix I/O monitoring capabilities */

#ifdef ASYNC_CAN_MONITOR_IO
static void
setUnixTransferResult (OperationEntry *operation, ssize_t count) {
  TransferExtension *extension = operation->extension;

  if (count == -1) {
    operation->error = errno;
  } else if (count == 0) {
    extension->direction.input.end = 1;
  } else {
    extension->length += count;
  }

  operation->finished = 1;
}

static void
finishUnixRead (OperationEntry *operation) {
  FunctionEntry *function = operation->function;
  TransferExtension *extension = operation->extension;
  int result = read(function->fileDescriptor,
                    &extension->buffer[extension->length],
                    extension->size - extension->length);

  setUnixTransferResult(operation, result);
}

static void
finishUnixWrite (OperationEntry *operation) {
  FunctionEntry *function = operation->function;
  TransferExtension *extension = operation->extension;
  int result = write(function->fileDescriptor,
                     &extension->buffer[extension->length],
                     extension->size - extension->length);

  setUnixTransferResult(operation, result);
}
#endif /* ASYNC_CAN_MONITOR_IO */
#endif /* __MINGW32__ */

#ifdef ASYNC_CAN_MONITOR_IO
static void
deallocateFunctionEntry (void *item, void *data) {
  FunctionEntry *function = item;

  if (function->operations) deallocateQueue(function->operations);
  if (function->methods->endFunction) function->methods->endFunction(function);
  free(function);
}

static Queue *
getFunctionQueue (int create) {
  AsyncThreadSpecificData *tsd = asyncGetThreadSpecificData();
  if (!tsd) return NULL;

  if (!tsd->functionQueue && create) {
    tsd->functionQueue = newQueue(deallocateFunctionEntry, NULL);
  }

  return tsd->functionQueue;
}

static int
invokeMonitorCallback (OperationEntry *operation) {
  MonitorExtension *extension = operation->extension;
  AsyncMonitorCallback *callback = extension->callback;

  if (callback) {
    const AsyncMonitorCallbackParameters parameters = {
      .data = operation->data
    };

    if (callback(&parameters)) return 1;
  }

  return 0;
}

static int
invokeInputCallback (OperationEntry *operation) {
  TransferExtension *extension = operation->extension;
  size_t count;

  if (!extension->direction.input.callback) return 0;

  {
    const AsyncInputCallbackParameters parameters = {
      .data = operation->data,
      .buffer = extension->buffer,
      .size = extension->size,
      .length = extension->length,
      .error = operation->error,
      .end = extension->direction.input.end
    };

    count = extension->direction.input.callback(&parameters);
  }

  if (operation->error) return 0;
  if (extension->direction.input.end) return 0;

  operation->finished = 0;
  if (count) {
    memmove(extension->buffer, &extension->buffer[count],
            extension->length -= count);
    if (extension->length > 0) operation->finished = 1;
  }

  return 1;
}

static int
invokeOutputCallback (OperationEntry *operation) {
  TransferExtension *extension = operation->extension;

  if (!operation->error && (extension->length < extension->size)) {
    operation->finished = 0;
    return 1;
  }

  if (extension->direction.output.callback) {
    const AsyncOutputCallbackParameters parameters = {
      .data = operation->data,
      .buffer = extension->buffer,
      .size = extension->size,
      .error = operation->error
    };

    extension->direction.output.callback(&parameters);
  }

  return 0;
}

static Element *
getActiveOperationElement (const FunctionEntry *function) {
  Queue *queue = function->operations;

  if (function->methods->invokeCallback == invokeMonitorCallback) return getQueueTail(queue);
  return getQueueHead(queue);
}

static OperationEntry *
getActiveOperation (const FunctionEntry *function) {
  Element *element = getActiveOperationElement(function);

  if (element) return getElementItem(element);
  return NULL;
}

static void
startOperation (OperationEntry *operation) {
  if (operation->function->methods->startOperation) {
    operation->function->methods->startOperation(operation);
  }
}

static void
finishOperation (OperationEntry *operation) {
  if (operation->function->methods->finishOperation) {
    operation->function->methods->finishOperation(operation);
  }
}

static int
addFunctionMonitor (void *item, void *data) {
  const FunctionEntry *function = item;
  MonitorGroup *monitors = data;
  OperationEntry *operation = getActiveOperation(function);

  if (operation) {
    operation->monitor = NULL;

    if (!operation->active) {
      if (operation->finished) {
        return 1;
      }

      operation->monitor = &monitors->array[monitors->count++];
      initializeMonitor(operation->monitor, function, operation);
    }
  }

  return 0;
}

static int
testFunctionMonitor (void *item, void *data) {
  FunctionEntry *function = item;
  OperationEntry *operation = getActiveOperation(function);

  if (operation) {
    if (operation->monitor) {
      if (testMonitor(operation->monitor, function)) {
        return 1;
      }
    }
  }

  return 0;
}

int
asyncHandleOperation (AsyncThreadSpecificData *tsd, long int timeout) {
  int handled = 0;
  Queue *functions = tsd->functionQueue;
  unsigned int functionCount = functions? getQueueSize(functions): 0;

  prepareMonitors();

  if (functionCount) {
    MonitorEntry monitorArray[functionCount];
    MonitorGroup monitors = {
      .array = monitorArray,
      .count = 0
    };
    Element *functionElement = processQueue(functions, addFunctionMonitor, &monitors);

    if (!functionElement) {
      if (!monitors.count) {
        approximateDelay(timeout);
      } else if (awaitMonitors(&monitors, timeout)) {
        functionElement = processQueue(functions, testFunctionMonitor, NULL);
      }
    }

    if (functionElement) {
      FunctionEntry *function = getElementItem(functionElement);
      Element *operationElement = getActiveOperationElement(function);
      OperationEntry *operation = getElementItem(operationElement);

      if (!operation->finished) finishOperation(operation);

      operation->active = 1;
      if (!function->methods->invokeCallback(operation)) operation->cancel = 1;
      operation->active = 0;
      handled = 1;

      if (operation->cancel) {
        deleteElement(operationElement);
      } else {
        operation->error = 0;
      }

      if ((operationElement = getActiveOperationElement(function))) {
        operation = getElementItem(operationElement);
        if (!operation->finished) startOperation(operation);
        requeueElement(functionElement);
      } else {
        deleteElement(functionElement);
      }
    }
  } else {
    approximateDelay(timeout);
  }

  return handled;
}

static void
deallocateOperationEntry (void *item, void *data) {
  OperationEntry *operation = item;
  if (operation->extension) free(operation->extension);
  free(operation);
}

static void
cancelOperation (Element *operationElement) {
  OperationEntry *operation = getElementItem(operationElement);

  if (operation->active) {
    operation->cancel = 1;
  } else {
    FunctionEntry *function = operation->function;
    int isFirstOperation = operationElement == getActiveOperationElement(function);

    if (isFirstOperation) {
      if (!operation->finished) {
        if (operation->function->methods->cancelOperation) {
          operation->function->methods->cancelOperation(operation);
        }
      }
    }

    if (getQueueSize(function->operations) == 1) {
      Queue *functionQueue = getFunctionQueue(0);
      Element *functionElement = findElementWithItem(functionQueue, function);

      deleteElement(functionElement);
    } else {
      deleteElement(operationElement);

      if (isFirstOperation) {
        operationElement = getActiveOperationElement(function);
        operation = getElementItem(operationElement);

        if (!operation->finished) startOperation(operation);
      }
    }
  }
}

static int
testFunctionEntry (const void *item, const void *data) {
  const FunctionEntry *function = item;
  const FunctionKey *key = data;
  return (function->fileDescriptor == key->fileDescriptor) &&
         (function->methods == key->methods);
}

static Element *
getFunctionElement (FileDescriptor fileDescriptor, const FunctionMethods *methods, int create) {
  Queue *functions = getFunctionQueue(create);
  if (functions) {
    {
      FunctionKey key;
      key.fileDescriptor = fileDescriptor;
      key.methods = methods;

      {
        Element *element = findElement(functions, testFunctionEntry, &key);
        if (element) return element;
      }
    }

    if (create) {
      FunctionEntry *function;

      if ((function = malloc(sizeof(*function)))) {
        function->fileDescriptor = fileDescriptor;
        function->methods = methods;

        if ((function->operations = newQueue(deallocateOperationEntry, NULL))) {
          {
            static AsyncQueueMethods methods = {
              .cancelRequest = cancelOperation
            };

            setQueueData(function->operations, &methods);
          }

          if (methods->beginFunction) methods->beginFunction(function);

          {
            Element *element = enqueueItem(functions, function);
            if (element) return element;
          }

          deallocateQueue(function->operations);
        }

        free(function);
      } else {
        logMallocError();
      }
    }
  }

  return NULL;
}

static Element *
newOperation (
  FileDescriptor fileDescriptor,
  const FunctionMethods *methods,
  void *extension,
  void *data
) {
  OperationEntry *operation;

  if ((operation = malloc(sizeof(*operation)))) {
    Element *functionElement;

    if ((functionElement = getFunctionElement(fileDescriptor, methods, 1))) {
      FunctionEntry *function = getElementItem(functionElement);
      int isFirstOperation = !getQueueSize(function->operations);
      Element *operationElement = enqueueItem(function->operations, operation);

      if (operationElement) {
        operation->function = function;
        operation->extension = extension;
        operation->data = data;

        operation->monitor = NULL;
        operation->error = 0;

        operation->active = 0;
        operation->cancel = 0;
        operation->finished = 0;

        if (isFirstOperation) startOperation(operation);
        return operationElement;
      }

      if (isFirstOperation) deleteElement(functionElement);
    }

    free(operation);
  } else {
    logMallocError();
  }

  return NULL;
}

typedef struct {
  FileDescriptor fileDescriptor;
  const FunctionMethods *methods;
  AsyncMonitorCallback *callback;
  void *data;
} MonitorFileOperationParameters;

static Element *
newFileMonitorOperation (const void *parameters) {
  const MonitorFileOperationParameters *mop = parameters;
  MonitorExtension *extension;

  if ((extension = malloc(sizeof(*extension)))) {
    extension->callback = mop->callback;

    {
      Element *element = newOperation(mop->fileDescriptor, mop->methods, extension, mop->data);

      if (element) return element;
    }

    free(extension);
  } else {
    logMallocError();
  }

  return NULL;
}

static Element *
newTransferOperation (
  FileDescriptor fileDescriptor,
  const FunctionMethods *methods,
  const TransferDirectionUnion *direction,
  size_t size, const void *buffer,
  void *data
) {
  TransferExtension *extension;

  if ((extension = malloc(sizeof(*extension) + size))) {
    extension->direction = *direction;
    extension->size = size;
    extension->length = 0;
    if (buffer) memcpy(extension->buffer, buffer, size);

    {
      Element *element = newOperation(fileDescriptor, methods, extension, data);

      if (element) return element;
    }

    free(extension);
  } else {
    logMallocError();
  }

  return NULL;
}

typedef struct {
  FileDescriptor fileDescriptor;
  size_t size;
  AsyncInputCallback *callback;
  void *data;
} InputOperationParameters;

static Element *
newInputOperation (const void *parameters) {
  const InputOperationParameters *iop = parameters;

  TransferDirectionUnion direction = {
    .input = {
      .callback = iop->callback,
      .end = 0
    }
  };

  static const FunctionMethods methods = {
    .functionName = "transferInput",

#ifdef __MINGW32__
    .beginFunction = beginWindowsFunction,
    .endFunction = endWindowsFunction,
    .startOperation = startWindowsRead,
    .finishOperation = finishWindowsTransferOperation,
    .cancelOperation = cancelWindowsTransferOperation,
#else /* __MINGW32__ */
    .beginFunction = beginUnixInputFunction,
    .finishOperation = finishUnixRead,
#endif /* __MINGW32__ */

    .invokeCallback = invokeInputCallback
  };

  return newTransferOperation(iop->fileDescriptor, &methods, &direction, iop->size, NULL, iop->data);
}

typedef struct {
  FileDescriptor fileDescriptor;
  size_t size;
  const void *buffer;
  AsyncOutputCallback *callback;
  void *data;
} OutputOperationParameters;

static Element *
newOutputOperation (const void *parameters) {
  const OutputOperationParameters *oop = parameters;

  TransferDirectionUnion direction = {
    .output = {
      .callback = oop->callback
    }
  };

  static const FunctionMethods methods = {
    .functionName = "transferOutput",

#ifdef __MINGW32__
    .beginFunction = beginWindowsFunction,
    .endFunction = endWindowsFunction,
    .startOperation = startWindowsWrite,
    .finishOperation = finishWindowsTransferOperation,
    .cancelOperation = cancelWindowsTransferOperation,
#else /* __MINGW32__ */
    .beginFunction = beginUnixOutputFunction,
    .finishOperation = finishUnixWrite,
#endif /* __MINGW32__ */

    .invokeCallback = invokeOutputCallback
  };

  return newTransferOperation(oop->fileDescriptor, &methods, &direction, oop->size, oop->buffer, oop->data);
}

#else /* ASYNC_CAN_MONITOR_IO */
int
asyncHandleOperation (AsyncThreadSpecificData *tsd, long int timeout) {
  approximateDelay(timeout);
  return 0;
}
#endif /* ASYNC_CAN_MONITOR_IO */

int
asyncMonitorFileInput (
  AsyncHandle *handle,
  FileDescriptor fileDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
#ifdef ASYNC_CAN_MONITOR_IO
  static const FunctionMethods methods = {
    .functionName = "monitorFileInput",

#ifdef __MINGW32__
    .beginFunction = beginWindowsFunction,
    .endFunction = endWindowsFunction,
#else /* __MINGW32__ */
    .beginFunction = beginUnixInputFunction,
#endif /* __MINGW32__ */

    .invokeCallback = invokeMonitorCallback
  };

  const MonitorFileOperationParameters mop = {
    .fileDescriptor = fileDescriptor,
    .methods = &methods,
    .callback = callback,
    .data = data
  };

  return asyncMakeHandle(handle, newFileMonitorOperation, &mop);
#else /* ASYNC_CAN_MONITOR_IO */
  logUnsupportedFunction();
  return 0;
#endif /* ASYNC_CAN_MONITOR_IO */
}

int
asyncMonitorFileOutput (
  AsyncHandle *handle,
  FileDescriptor fileDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
#ifdef ASYNC_CAN_MONITOR_IO
  static const FunctionMethods methods = {
    .functionName = "monitorFileOutput",

#ifdef __MINGW32__
    .beginFunction = beginWindowsFunction,
    .endFunction = endWindowsFunction,
#else /* __MINGW32__ */
    .beginFunction = beginUnixOutputFunction,
#endif /* __MINGW32__ */

    .invokeCallback = invokeMonitorCallback
  };

  const MonitorFileOperationParameters mop = {
    .fileDescriptor = fileDescriptor,
    .methods = &methods,
    .callback = callback,
    .data = data
  };

  return asyncMakeHandle(handle, newFileMonitorOperation, &mop);
#else /* ASYNC_CAN_MONITOR_IO */
  logUnsupportedFunction();
  return 0;
#endif /* ASYNC_CAN_MONITOR_IO */
}

int
asyncMonitorFileAlert (
  AsyncHandle *handle,
  FileDescriptor fileDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
#ifdef ASYNC_CAN_MONITOR_IO
  static const FunctionMethods methods = {
    .functionName = "monitorFileAlert",

#ifdef __MINGW32__
    .beginFunction = beginWindowsFunction,
    .endFunction = endWindowsFunction,
#else /* __MINGW32__ */
    .beginFunction = beginUnixAlertFunction,
#endif /* __MINGW32__ */

    .invokeCallback = invokeMonitorCallback
  };

  const MonitorFileOperationParameters mop = {
    .fileDescriptor = fileDescriptor,
    .methods = &methods,
    .callback = callback,
    .data = data
  };

  return asyncMakeHandle(handle, newFileMonitorOperation, &mop);
#else /* ASYNC_CAN_MONITOR_IO */
  logUnsupportedFunction();
  return 0;
#endif /* ASYNC_CAN_MONITOR_IO */
}

int
asyncReadFile (
  AsyncHandle *handle,
  FileDescriptor fileDescriptor,
  size_t size,
  AsyncInputCallback *callback, void *data
) {
#ifdef ASYNC_CAN_MONITOR_IO
  const InputOperationParameters iop = {
    .fileDescriptor = fileDescriptor,
    .size = size,
    .callback = callback,
    .data = data
  };

  return asyncMakeHandle(handle, newInputOperation, &iop);
#else /* ASYNC_CAN_MONITOR_IO */
  logUnsupportedFunction();
  return 0;
#endif /* ASYNC_CAN_MONITOR_IO */
}

int
asyncWriteFile (
  AsyncHandle *handle,
  FileDescriptor fileDescriptor,
  const void *buffer, size_t size,
  AsyncOutputCallback *callback, void *data
) {
#ifdef ASYNC_CAN_MONITOR_IO
  const OutputOperationParameters oop = {
    .fileDescriptor = fileDescriptor,
    .size = size,
    .buffer = buffer,
    .callback = callback,
    .data = data
  };

  return asyncMakeHandle(handle, newOutputOperation, &oop);
#else /* ASYNC_CAN_MONITOR_IO */
  logUnsupportedFunction();
  return 0;
#endif /* ASYNC_CAN_MONITOR_IO */
}

#ifdef __MINGW32__
int
asyncMonitorSocketInput (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
  logUnsupportedFunction();
  return 0;
}

int
asyncMonitorSocketOutput (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
  logUnsupportedFunction();
  return 0;
}

int
asyncMonitorSocketAlert (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
  logUnsupportedFunction();
  return 0;
}

int
asyncReadSocket (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  size_t size,
  AsyncInputCallback *callback, void *data
) {
  logUnsupportedFunction();
  return 0;
}

int
asyncWriteSocket (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  const void *buffer, size_t size,
  AsyncOutputCallback *callback, void *data
) {
  logUnsupportedFunction();
  return 0;
}

#else /* __MINGW32__ */
int
asyncMonitorSocketInput (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
  return asyncMonitorFileInput(handle, socketDescriptor, callback, data);
}

int
asyncMonitorSocketOutput (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
  return asyncMonitorFileOutput(handle, socketDescriptor, callback, data);
}

int
asyncMonitorSocketAlert (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  AsyncMonitorCallback *callback, void *data
) {
  return asyncMonitorFileAlert(handle, socketDescriptor, callback, data);
}

int
asyncReadSocket (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  size_t size,
  AsyncInputCallback *callback, void *data
) {
  return asyncReadFile(handle, socketDescriptor, size, callback, data);
}

int
asyncWriteSocket (
  AsyncHandle *handle,
  SocketDescriptor socketDescriptor,
  const void *buffer, size_t size,
  AsyncOutputCallback *callback, void *data
) {
  return asyncWriteFile(handle, socketDescriptor, buffer, size, callback, data);
}
#endif /* __MINGW32__ */
