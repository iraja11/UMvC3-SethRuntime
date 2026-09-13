#ifndef SETH_WIN_API_H
#define SETH_WIN_API_H
#include "probe_core.h"
/* Just the Win64 signatures of the Kernel32 functions we call - no SDK, no CRT. */
typedef void *HANDLE;
typedef U32 DWORD;
typedef int BOOL;
#define API __declspec(dllimport)
API HANDLE GetModuleHandleA(const char *);
API DWORD GetModuleFileNameA(HANDLE,char *,DWORD);
API HANDLE GetCurrentProcess(void);
API BOOL ReadProcessMemory(HANDLE,const void *,void *,U64,U64 *);
API BOOL VirtualProtect(void *,U64,DWORD,DWORD *);
API BOOL FlushInstructionCache(HANDLE,const void *,U64);
API BOOL DisableThreadLibraryCalls(HANDLE);
API HANDLE CreateFileA(const char *,DWORD,DWORD,void *,DWORD,DWORD,HANDLE);
API BOOL WriteFile(HANDLE,const void *,DWORD,DWORD *,void *);
API BOOL ReadFile(HANDLE,void *,DWORD,DWORD *,void *);
API BOOL CloseHandle(HANDLE);
API BOOL GetModuleHandleExA(DWORD,const char *,HANDLE *);
API void *VirtualAlloc(void *,U64,DWORD,DWORD);
#endif
