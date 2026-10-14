#include <iostream>
#include <cstdint>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>

#define dwEntityList 0x4D44A04
#define m_bSpotted 0x93D

DWORD dwPid;
HANDLE hProcess;
uintptr_t client; //uintptr_t instead of DWORD so the base address is not truncated in an x64 build.

uintptr_t GetModule(const char* modName, DWORD procId) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, procId);
    if (hSnap == INVALID_HANDLE_VALUE)
        return 0;

    uintptr_t modBaseAddr = 0;
    MODULEENTRY32 modEntry;
    modEntry.dwSize = sizeof(modEntry);
    if (Module32First(hSnap, &modEntry)) {
        do {
            if (!strcmp(modEntry.szModule, modName)) {
                modBaseAddr = (uintptr_t)modEntry.modBaseAddr;
                break;
            }
        } while (Module32Next(hSnap, &modEntry));
    }
    CloseHandle(hSnap); //Closed on every path, the snapshot used to leak when the module was missing.
    return modBaseAddr; //0 means "not found", the caller must check it.
}

template<typename T> T RPM(SIZE_T address) {
    T buffer; ReadProcessMemory(hProcess, (void*)address, &buffer, sizeof(T), nullptr);
    return buffer;
}

template<typename T> void WPM(SIZE_T address, T buffer) {
    WriteProcessMemory(hProcess, (void*)address, &buffer, sizeof(T), nullptr);
}

int main() { //"void main" is a Microsoft extension, the standard requires int.
    HWND hwnd = FindWindowA(0, "Counter-Strike: Global Offensive");
    if (!hwnd) {
        std::cout << "Game window not found." << std::endl;
        return 1;
    }

    GetWindowThreadProcessId(hwnd, &dwPid);
    hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwPid);
    if (!hProcess) {
        std::cout << "OpenProcess failed, error " << GetLastError() << std::endl;
        return 1;
    }

    client = GetModule("client.dll", dwPid);
    if (!client) {
        std::cout << "client.dll not found in the target process." << std::endl;
        CloseHandle(hProcess);
        return 1;
    }

    while (!GetAsyncKeyState(VK_END)) { //Press "end" to stop, the loop used to run forever.
        for (int i = 1; i < 64; i++) {
            DWORD dwCurrentEntity = RPM<DWORD>(client + dwEntityList + i * 0x10);
            if (dwCurrentEntity) {
                WPM<bool>(dwCurrentEntity + m_bSpotted, true);
            }
        }
        Sleep(50);
    }

    CloseHandle(hProcess);
    return 0;
}
