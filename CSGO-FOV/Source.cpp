#include <iostream>
#include <cstdint>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>

#define dwEntityList 0x4D42A34
#define m_iDefaultFOV 0x332C

uintptr_t moduleBase;
DWORD procId;
HWND hwnd;
HANDLE hProcess;

uintptr_t GetModuleBaseAddress(const char* modName) {
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
	T buffer;
	ReadProcessMemory(hProcess, (LPCVOID)address, &buffer, sizeof(T), NULL);
	return buffer;
}

template<typename T> void WPM(SIZE_T address, T buffer) {
	WriteProcessMemory(hProcess, (LPVOID)address, &buffer, sizeof(buffer), NULL);
}

int main() { //"void main" is a Microsoft extension, the standard requires int.
	hwnd = FindWindowA(NULL, "Counter-Strike: Global Offensive");
	if (!hwnd) {
		std::cout << "Game window not found." << std::endl;
		return 1;
	}

	GetWindowThreadProcessId(hwnd, &procId);
	hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, procId);
	if (!hProcess) {
		std::cout << "OpenProcess failed, error " << GetLastError() << std::endl;
		return 1;
	}

	moduleBase = GetModuleBaseAddress("client.dll");
	if (!moduleBase) {
		std::cout << "client.dll not found in the target process." << std::endl;
		CloseHandle(hProcess);
		return 1;
	}

	int fov = 90;
	int lastPrintedFOV = -1;

	while (!GetAsyncKeyState(VK_END))
	{
		uintptr_t localPlayer = RPM<uintptr_t>(moduleBase + dwEntityList);
		int iFOV = RPM<int>(localPlayer + m_iDefaultFOV);
		if (iFOV != lastPrintedFOV) { //Only print on change, the old loop flooded the console.
			std::cout << "FOV: " << iFOV << std::endl;
			lastPrintedFOV = iFOV;
		}

		if (GetAsyncKeyState(0x76 /*F7*/) & 1)
		{
			//minus
			fov = fov - 1;
			WPM<int>(localPlayer + m_iDefaultFOV, fov);
		}

		if (GetAsyncKeyState(0x77 /*F8*/) & 1)
		{
			//add
			fov = fov + 1;
			WPM<int>(localPlayer + m_iDefaultFOV, fov);
		}

		if (GetAsyncKeyState(0x78 /*F9*/) & 1)
		{
			//resets
			fov = 90;
			WPM<int>(localPlayer + m_iDefaultFOV, fov);
		}

		Sleep(10); //The loop used to spin a core at 100%.
	}

	CloseHandle(hProcess);
	return 0;
}
