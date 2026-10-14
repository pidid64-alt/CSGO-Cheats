#include <iostream>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>
//The old "#include "Offsets.h"" was removed: that header is not in the repository, which is why
//this project never compiled. dwViewMatrix lived in it and is defined with the others below.

#define dwLocalPlayer 0xD30B94
#define dwEntityList 0x4D44A24
#define dwViewMatrix 0x4D2E014 //Taken from CSGO-GDI-ESP. Like every offset here it is dead, and it comes from a different dump than the lines above, so re-dump all of them together before trusting any of it.
#define m_dwBoneMatrix 0x26A8
#define m_iTeamNum 0xF4
#define m_iHealth 0x100
#define m_vecOrigin 0x138
#define m_bDormant 0xED

const int SCREEN_WIDTH = GetSystemMetrics(SM_CXSCREEN); const int xhairx = SCREEN_WIDTH / 2;
const int SCREEN_HEIGHT = GetSystemMetrics(SM_CYSCREEN); const int xhairy = SCREEN_HEIGHT / 2;

HWND hwnd;
DWORD procId;
HANDLE hProcess;
uintptr_t moduleBase;
HDC hdc;
std::atomic<int> closest{ 1 }; //Written by a worker thread and read by main, so it has to be atomic.

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

class Vector3 {
public:
	float x, y, z;
	Vector3() : x(0.f), y(0.f), z(0.f) {}
	Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};


int getTeam(uintptr_t player) {
	return RPM<int>(player + m_iTeamNum);
}

uintptr_t GetLocalPlayer() {
	return RPM<uintptr_t>(moduleBase + dwLocalPlayer);
}

uintptr_t GetPlayer(int index) {  //Each player has an index. 1-64
	return RPM<uintptr_t>(moduleBase + dwEntityList + index * 0x10); //We multiply the index by 0x10 to select the player we want in the entity list.
}

int GetPlayerHealth(uintptr_t player) {
	return RPM<int>(player + m_iHealth);
}

Vector3 PlayerLocation(uintptr_t player) { //Stores XYZ coordinates in a Vector3.
	return RPM<Vector3>(player + m_vecOrigin);
}

bool DormantCheck(uintptr_t player) {
	return RPM<int>(player + m_bDormant) != 0;
}

Vector3 get_head(uintptr_t player) {
	struct boneMatrix_t {
		BYTE pad3[12];
		float x;
		BYTE pad1[12];
		float y;
		BYTE pad2[12];
		float z;
	};
	uintptr_t boneBase = RPM<uintptr_t>(player + m_dwBoneMatrix);
	boneMatrix_t boneMatrix = RPM<boneMatrix_t>(boneBase + (sizeof(boneMatrix_t) * 8 /*8 is the boneid for head*/));
	return Vector3(boneMatrix.x, boneMatrix.y, boneMatrix.z);
}

struct view_matrix_t {
	float matrix[16];
} vm;

struct Vector3 WorldToScreen(const struct Vector3 pos, struct view_matrix_t matrix) { //This turns 3D coordinates (ex: XYZ) int 2D coordinates (ex: XY).
	struct Vector3 out;
	float _x = matrix.matrix[0] * pos.x + matrix.matrix[1] * pos.y + matrix.matrix[2] * pos.z + matrix.matrix[3];
	float _y = matrix.matrix[4] * pos.x + matrix.matrix[5] * pos.y + matrix.matrix[6] * pos.z + matrix.matrix[7];
	out.z = matrix.matrix[12] * pos.x + matrix.matrix[13] * pos.y + matrix.matrix[14] * pos.z + matrix.matrix[15];

	_x *= 1.f / out.z;
	_y *= 1.f / out.z;

	out.x = SCREEN_WIDTH * .5f;
	out.y = SCREEN_HEIGHT * .5f;

	out.x += 0.5f * _x * SCREEN_WIDTH + 0.5f;
	out.y -= 0.5f * _y * SCREEN_HEIGHT + 0.5f;

	return out;
}

float pythag(float x1, float y1, float x2, float y2) { //Floats, the callers pass screen coordinates that are not whole numbers.
	return std::sqrt(std::pow(x2 - x1, 2.f) + std::pow(y2 - y1, 2.f));
}

int FindClosestEnemy() {
	float Finish;
	int ClosestEntity = 1;
	float Closest = FLT_MAX;
	int localTeam = getTeam(GetLocalPlayer());
	for (int i = 1; i < 64; i++) { //Loops through all the entitys in the index 1-64.
		uintptr_t Entity = GetPlayer(i);
		int EnmTeam = getTeam(Entity); if (EnmTeam == localTeam) continue;
		int EnmHealth = GetPlayerHealth(Entity); if (EnmHealth < 1 || EnmHealth > 100) continue;
		bool Dormant = DormantCheck(Entity); if (Dormant) continue;
		Vector3 headBone = WorldToScreen(get_head(Entity), vm);
		Finish = pythag(headBone.x, headBone.y, (float)xhairx, (float)xhairy);
		if (Finish < Closest) {
			Closest = Finish;
			ClosestEntity = i;
		}
	}

	return ClosestEntity;
}

void DrawLine(float StartX, float StartY, float EndX, float EndY) { //This function is optional for debugging.
	HPEN hNPen = CreatePen(PS_SOLID, 2, 0x0000FF /*red*/);
	HPEN hOPen = (HPEN)SelectObject(hdc, hNPen);
	MoveToEx(hdc, (int)StartX, (int)StartY, NULL); //start of line
	LineTo(hdc, (int)EndX, (int)EndY); //end of line
	DeleteObject(SelectObject(hdc, hOPen)); //Puts the old pen back and deletes the one we created.
}

DWORD WINAPI FindClosestEnemyThread(LPVOID) { //Real thread signature, the old one was cast to LPTHREAD_START_ROUTINE.
	while (true) {
		closest = FindClosestEnemy();
		Sleep(1); //The thread used to spin a core at 100%.
	}
	return 0;
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

	hdc = GetDC(hwnd);
	if (!hdc) {
		std::cout << "GetDC failed." << std::endl;
		CloseHandle(hProcess);
		return 1;
	}

	HANDLE hThread = CreateThread(NULL, 0, FindClosestEnemyThread, NULL, 0, NULL);
	if (!hThread) {
		std::cout << "CreateThread failed, error " << GetLastError() << std::endl;
		ReleaseDC(hwnd, hdc);
		CloseHandle(hProcess);
		return 1;
	}

	while (!GetAsyncKeyState(VK_END)) { //press the "end" key to end the hack
		vm = RPM<view_matrix_t>(moduleBase + dwViewMatrix);
		Vector3 closestw2shead = WorldToScreen(get_head(GetPlayer(closest.load())), vm);
		DrawLine((float)xhairx, (float)xhairy, closestw2shead.x, closestw2shead.y); //optinal for debugging

		if (GetAsyncKeyState(VK_MENU /*alt key*/) && closestw2shead.z >= 0.001f /*onscreen check*/)
			SetCursorPos((int)closestw2shead.x, (int)closestw2shead.y); //turn off "raw input" in CSGO settings

		Sleep(1); //The loop used to spin a core at 100%.
	}

	CloseHandle(hThread);
	ReleaseDC(hwnd, hdc);
	CloseHandle(hProcess);
	return 0;
}
