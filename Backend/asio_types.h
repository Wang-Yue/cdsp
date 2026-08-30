#ifndef CLIB_BACKEND_ASIO_TYPES_H
#define CLIB_BACKEND_ASIO_TYPES_H

#if defined(ENABLE_ASIO)

#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <stdbool.h>
#include <stdint.h>
#include <unknwn.h>
#include <windows.h>

#include "Config/engine_config_types.h"

// COM Release helper
#define SAFE_RELEASE(punk)         \
  if ((punk) != NULL) {            \
    (punk)->lpVtbl->Release(punk); \
    (punk) = NULL;                 \
  }

// ASIO basic types matching azo-sys
typedef int32_t ASIOBool;
#define ASIOFalse 0
#define ASIOTrue 1

typedef double ASIOSampleRate;
typedef long ASIOError;

// ASIO sample types matching azo-sys / utils.rs
#define ASIO_ST_INT16_MSB 0
#define ASIO_ST_INT24_MSB 1
#define ASIO_ST_INT32_MSB 2
#define ASIO_ST_FLOAT32_MSB 3
#define ASIO_ST_FLOAT64_MSB 4
#define ASIO_ST_INT32_MSB_16 8
#define ASIO_ST_INT32_MSB_18 9
#define ASIO_ST_INT32_MSB_20 10
#define ASIO_ST_INT32_MSB_24 11
#define ASIO_ST_INT16_LSB 16
#define ASIO_ST_INT24_LSB 17
#define ASIO_ST_INT32_LSB 18
#define ASIO_ST_FLOAT32_LSB 19
#define ASIO_ST_FLOAT64_LSB 20
#define ASIO_ST_INT32_LSB_16 24
#define ASIO_ST_INT32_LSB_18 25
#define ASIO_ST_INT32_LSB_20 26
#define ASIO_ST_INT32_LSB_24 27
#define ASIO_ST_DSD_INT8_LSB_1 32
#define ASIO_ST_DSD_INT8_MSB_1 33
#define ASIO_ST_DSD_INT8_NER8 40

// Standard ASIO message selectors matching azo-sys MessageSelector
#define K_ASIO_SELECTOR_SUPPORTED 1
#define K_ASIO_ENGINE_VERSION 2
#define K_ASIO_RESET_REQUEST 3
#define K_ASIO_BUFFER_SIZE_CHANGE 4
#define K_ASIO_RESYNC_REQUEST 5
#define K_ASIO_LATENCIES_CHANGED 6
#define K_ASIO_SUPPORTS_TIME_INFO 7
#define K_ASIO_SUPPORTS_TIME_CODE 8

// ASIO DSD Future Selectors (Steinberg ASIO SDK 2.3 / azo-sys FutureSelector)
#define kAsioSetIoFormat 0x23111961L
#define kAsioGetIoFormat 0x23111983L
#define kAsioCanDoIoFormat 0x23112004L
#define ASE_SUCCESS 0x3f4847a0L

typedef enum {
  kASIOFormatInvalid = -1,
  kASIOFormatPCM = 0,
  kASIOFormatDSD = 1
} ASIOSampleFormatType;

typedef struct {
  ASIOSampleFormatType FormatType;
  char future[512 - sizeof(ASIOSampleFormatType)];
} ASIOIoFormat;

typedef struct {
  int32_t channel;
  ASIOBool isInput;
  ASIOBool isActive;
  int32_t channelGroup;
  int32_t type;
  char name[32];
} ASIOChannelInfo;

typedef struct {
  ASIOBool isInput;
  int32_t channelNum;
  void* buffers[2];
} ASIOBufferInfo;

typedef struct IASIO IASIO;

typedef void (*ASIOCallbackBufferSwitch)(long doubleBufferIndex,
                                         ASIOBool directProcess);
typedef void (*ASIOCallbackSampleRateDidChange)(ASIOSampleRate sRate);
typedef long (*ASIOCallbackAsioMessage)(long selector, long value,
                                        void* message, double* opt);
typedef void* (*ASIOCallbackBufferSwitchTimeInfo)(void* params,
                                                  long doubleBufferIndex,
                                                  ASIOBool directProcess);

typedef struct {
  ASIOCallbackBufferSwitch bufferSwitch;
  ASIOCallbackSampleRateDidChange sampleRateDidChange;
  ASIOCallbackAsioMessage asioMessage;
  ASIOCallbackBufferSwitchTimeInfo bufferSwitchTimeInfo;
} ASIOCallbacks;

typedef struct IASIOVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IASIO* This, REFIID riid,
                                             void** ppvObject);
  ULONG(STDMETHODCALLTYPE* AddRef)(IASIO* This);
  ULONG(STDMETHODCALLTYPE* Release)(IASIO* This);
  ASIOBool(STDMETHODCALLTYPE* init)(IASIO* This, void* sysHandle);
  void(STDMETHODCALLTYPE* getDriverName)(IASIO* This, char* name);
  long(STDMETHODCALLTYPE* getDriverVersion)(IASIO* This);
  void(STDMETHODCALLTYPE* getErrorMessage)(IASIO* This, char* string);
  ASIOError(STDMETHODCALLTYPE* start)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* stop)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* getChannels)(IASIO* This, long* numInputChannels,
                                            long* numOutputChannels);
  ASIOError(STDMETHODCALLTYPE* getLatencies)(IASIO* This, long* inputLatency,
                                             long* outputLatency);
  ASIOError(STDMETHODCALLTYPE* getBufferSize)(IASIO* This, long* minSize,
                                              long* maxSize,
                                              long* preferredSize,
                                              long* granularity);
  ASIOError(STDMETHODCALLTYPE* canSampleRate)(IASIO* This,
                                              ASIOSampleRate sampleRate);
  ASIOError(STDMETHODCALLTYPE* getSampleRate)(IASIO* This,
                                              ASIOSampleRate* sampleRate);
  ASIOError(STDMETHODCALLTYPE* setSampleRate)(IASIO* This,
                                              ASIOSampleRate sampleRate);
  ASIOError(STDMETHODCALLTYPE* getClockSources)(IASIO* This, void* clocks,
                                                long* numSources);
  ASIOError(STDMETHODCALLTYPE* setClockSource)(IASIO* This, long reference);
  ASIOError(STDMETHODCALLTYPE* getSamplePosition)(IASIO* This, int64_t* sPos,
                                                  int64_t* tStamp);
  ASIOError(STDMETHODCALLTYPE* getChannelInfo)(IASIO* This,
                                               ASIOChannelInfo* info);
  ASIOError(STDMETHODCALLTYPE* createBuffers)(IASIO* This,
                                              ASIOBufferInfo* bufferInfos,
                                              long numChannels, long bufferSize,
                                              ASIOCallbacks* callbacks);
  ASIOError(STDMETHODCALLTYPE* disposeBuffers)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* controlPanel)(IASIO* This);
  ASIOError(STDMETHODCALLTYPE* future)(IASIO* This, long selector, void* opt);
  ASIOError(STDMETHODCALLTYPE* outputReady)(IASIO* This);
} IASIOVtbl;

struct IASIO {
  const IASIOVtbl* lpVtbl;
};

#endif  // ENABLE_ASIO

#endif  // CLIB_BACKEND_ASIO_TYPES_H
