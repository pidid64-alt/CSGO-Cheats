#include <iostream>
#include <cstdint>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>
//The old "#include "Offsets.h"" was removed: that header is not in the repository and every
//offset this file needs is defined right below.

#define dwLocalPlayer 0xD30B94
#define dwGlowObjectManager 0x528C8D8
#define dwEntityList 0x4D44A24
#define m_iGlowIndex 0xA428
#define m_iTeamNum 0xF4
#define m_iHealth 0x100
#define m_bDormant 0xED

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

//The padding members are zero initialised: they are written into the game as part of the struct,
//so leaving them uninitialised meant sending whatever happened to be on the stack.
struct glowStructEnemy {
	float red = 1.f;
	float green = 0.f;
	float blue = 0.f;
	float alpha = 1.f;
	uint8_t padding[8] = {};
	float unknown = 1.f;
	uint8_t padding2[4] = {};
	BYTE renderOccluded = true;
	BYTE renderUnoccluded = false;
	BYTE fullBloom = false;
}glowEnm;

struct glowStructLocal {
	float red = 0.f;
	float green = 1.f;
	float blue = 0.f;
	float alpha = 1.f;
	uint8_t padding[8] = {};
	float unknown = 1.f;
	uint8_t padding2[4] = {};
	BYTE renderOccluded = true;
	BYTE renderUnoccluded = false;
	BYTE fullBloom = false;
}glowLocal;

uintptr_t getLocalPlayer() {
	return RPM<uintptr_t>(moduleBase + dwLocalPlayer);
}

int main() {
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

	while (!GetAsyncKeyState(VK_END))
	{
		uintptr_t dwGlowManager = RPM<uintptr_t>(moduleBase + dwGlowObjectManager);
		int LocalTeam = RPM<int>(getLocalPlayer() + m_iTeamNum);
		for (int i = 1; i < 32; i++) {
			uintptr_t dwEntity = RPM<uintptr_t>(moduleBase + dwEntityList + i * 0x10);
			if (!dwEntity) continue; //Empty slots used to be dereferenced anyway.
			int iGlowIndx = RPM<int>(dwEntity + m_iGlowIndex);
			int EnmHealth = RPM<int>(dwEntity + m_iHealth); if (EnmHealth < 1 || EnmHealth > 100) continue;
			int Dormant = RPM<int>(dwEntity + m_bDormant); if (Dormant) continue;
			int EntityTeam = RPM<int>(dwEntity + m_iTeamNum);

			if (LocalTeam == EntityTeam)
			{
				WPM<glowStructLocal>(dwGlowManager + (iGlowIndx * 0x38) + 0x4, glowLocal);
			}
			else
			{
				WPM<glowStructEnemy>(dwGlowManager + (iGlowIndx * 0x38) + 0x4, glowEnm);
			}
		}

		Sleep(1); //The loop used to spin a core at 100%.
	}

	CloseHandle(hProcess);
	return 0;
}
