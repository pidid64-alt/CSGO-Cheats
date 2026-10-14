#include <iostream>
#include <cstdint>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>

#define m_iTeamNum 0xF4
#define dwLocalPlayer 0xD29B0C
#define dwEntityList 0x4D3D6AC
#define m_iCrosshairId 0xB3D4

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

uintptr_t getLocalPlayer() { //This will get the address to localplayer. 
	return RPM<uintptr_t>(moduleBase + dwLocalPlayer);
}

uintptr_t getPlayer(int index) {  //Each player in the game has an index.
	return RPM<uintptr_t>(moduleBase + dwEntityList + index * 0x10); //We use index times 0x10 because the distance between each player 0x10.
}

int getTeam(uintptr_t player) {
	return RPM<int>(player + m_iTeamNum);
}

int getCrosshairID(uintptr_t player) {
	return RPM<int>(player + m_iCrosshairId);
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

	while (!GetAsyncKeyState(VK_END)) {
		int CrosshairID = getCrosshairID(getLocalPlayer());
		if (CrosshairID <= 0 || CrosshairID >= 32) { //Range checked first, the old code read an entity before validating the id.
			Sleep(1);
			continue;
		}

		int CrosshairTeam = getTeam(getPlayer(CrosshairID - 1));
		int LocalTeam = getTeam(getLocalPlayer());
		if (LocalTeam != CrosshairTeam)
		{
			if (GetAsyncKeyState(VK_MENU /*alt key*/))
			{
				mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); //0 instead of NULL, these parameters are integers and not pointers.
				mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
				Sleep(100); //Optional
			}
		}

		Sleep(1); //The loop used to spin a core at 100%.
	}

	CloseHandle(hProcess);
	return 0;
}
