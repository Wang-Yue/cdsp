#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <dbghelp.h>
#include <tchar.h>
#include <windows.h>

inline void logCallstack(std::ofstream& crashLog) {
    uintptr_t baseAddr = (uintptr_t)GetModuleHandleA(NULL);
    crashLog << "Module Base Address: 0x" << std::hex << baseAddr << std::endl;

    void* stack[64];
    USHORT frames = RtlCaptureStackBackTrace(0, 64, stack, NULL);
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(1, sizeof(SYMBOL_INFO) + 256 * sizeof(char));
    if (symbol) {
        symbol->MaxNameLen = 255;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD displacement = 0;

        crashLog << "Callstack (" << frames << " frames):" << std::endl;
        for (USHORT i = 0; i < frames; i++) {
            uintptr_t address = (uintptr_t)(stack[i]);
            uintptr_t rva = address - baseAddr;
            uintptr_t preferredAddress = 0x140000000 + rva;

            crashLog << "  [" << i << "] Absolute: 0x" << std::hex << address << " | RVA: +0x" << std::hex << rva
                     << " | PreferredAddr: 0x" << std::hex << preferredAddress;

            if (SymFromAddr(process, address, 0, symbol)) {
                crashLog << " (" << symbol->Name << ")";
                if (SymGetLineFromAddr64(process, address, &displacement, &line)) {
                    crashLog << " [" << line.FileName << ":" << line.LineNumber << "]";
                }
            }
            crashLog << std::endl;
        }
        free(symbol);
    }
}

inline LONG WINAPI customUnhandledExceptionFilter(EXCEPTION_POINTERS* pExceptionInfo) {
    std::ofstream crashLog("crash_log.txt", std::ios::out | std::ios::app);
    crashLog << "========================================" << std::endl;
    crashLog << "CRASH DETECTED AT RUNTIME!" << std::endl;
    crashLog << "Exception Code    : 0x" << std::hex << pExceptionInfo->ExceptionRecord->ExceptionCode << std::endl;
    crashLog << "Exception Flags   : 0x" << std::hex << pExceptionInfo->ExceptionRecord->ExceptionFlags << std::endl;
    crashLog << "Exception Address : 0x" << std::hex << (uintptr_t)pExceptionInfo->ExceptionRecord->ExceptionAddress
             << std::endl;

    if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        crashLog << "Access Violation Details:" << std::endl;
        crashLog << "  Attempted to "
                 << (pExceptionInfo->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from")
                 << " memory address 0x" << std::hex << pExceptionInfo->ExceptionRecord->ExceptionInformation[1]
                 << std::endl;
    }

    logCallstack(crashLog);
    crashLog << "========================================" << std::endl;
    crashLog.close();

    // Generate Windows Minidump file (.dmp) for WinDbg / GDB analysis
    HANDLE hFile = CreateFileA("crash_dump.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = pExceptionInfo;
        mdei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mdei, NULL, NULL);
        CloseHandle(hFile);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

inline void signalHandler(int sig) {
    std::ofstream crashLog("crash_log.txt", std::ios::out | std::ios::app);
    crashLog << "========================================" << std::endl;
    crashLog << "FATAL SIGNAL RECEIVED: " << sig << std::endl;
#if defined(_WIN32)
    logCallstack(crashLog);
    HANDLE hFile = CreateFileA("crash_dump.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = NULL;
        mdei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mdei, NULL, NULL);
        CloseHandle(hFile);
    }
#endif
    crashLog << "========================================" << std::endl;
    crashLog.close();
    std::exit(sig);
}

inline void installCrashHandler() {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(customUnhandledExceptionFilter);
#endif
    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGFPE, signalHandler);
    std::signal(SIGILL, signalHandler);
}

#endif // CRASH_HANDLER_H
