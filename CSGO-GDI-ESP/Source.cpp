#include <iostream>
#include <cstdint>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>

#define dwEntityList 0x4D3C5FC
#define dwViewMatrix 0x4D2E014
#define m_iTeamNum 0xF4
#define m_iHealth 0x100
#define m_vecOrigin 0x138

uintptr_t moduleBase;
HANDLE TargetProcess;
HPEN BoxPen; //Created in main, GDI calls in a global initialiser run before the program is really up.
RECT WBounds;
HWND EspHWND;

template<typename T> T RPM(SIZE_T address) {
    T buffer;
    ReadProcessMemory(TargetProcess, (LPCVOID)address, &buffer, sizeof(T), NULL);
    return buffer;
}

//The old version looped forever when the module was missing, which hung the program with no output.
uintptr_t GetModuleBaseAddress(DWORD dwPid, const char* moduleName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, dwPid);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;

    uintptr_t dwBase = 0;
    MODULEENTRY32 ModuleEntry32;
    ModuleEntry32.dwSize = sizeof(MODULEENTRY32);
    if (Module32First(hSnapshot, &ModuleEntry32)) {
        do {
            if (!strcmp(ModuleEntry32.szModule, moduleName)) {
                dwBase = (uintptr_t)ModuleEntry32.modBaseAddr; //No DWORD cast, that truncated the address in an x64 build.
                break;
            }
        } while (Module32Next(hSnapshot, &ModuleEntry32));
    }
    CloseHandle(hSnapshot);
    return dwBase; //0 means "not found", the caller must check it.
}

struct Vector3 {
    float x, y, z;
};

struct view_matrix_t {
    float matrix[16];
};

struct Vector3 WorldToScreen(const struct Vector3 pos, struct view_matrix_t matrix) {
    struct Vector3 out;
    float _x = matrix.matrix[0] * pos.x + matrix.matrix[1] * pos.y + matrix.matrix[2] * pos.z + matrix.matrix[3];
    float _y = matrix.matrix[4] * pos.x + matrix.matrix[5] * pos.y + matrix.matrix[6] * pos.z + matrix.matrix[7];
    out.z = matrix.matrix[12] * pos.x + matrix.matrix[13] * pos.y + matrix.matrix[14] * pos.z + matrix.matrix[15];

    _x *= 1.f / out.z;
    _y *= 1.f / out.z;

    int width = WBounds.right - WBounds.left;
    int height = WBounds.bottom - WBounds.top; //Was "bottom + left": a typo that only worked because a client rect starts at 0,0.

    out.x = width * .5f;
    out.y = height * .5f;

    out.x += 0.5f * _x * width + 0.5f;
    out.y -= 0.5f * _y * height + 0.5f;

    return out;
}

void Draw(HDC hdc, Vector3 foot, Vector3 head) {
    float height = head.y - foot.y;
    float width = height / 2.4f;
    HGDIOBJ oldPen = SelectObject(hdc, BoxPen);
    Rectangle(hdc, (int)(foot.x - (width / 2)), (int)foot.y, (int)(head.x + (width / 2)), (int)head.y);
    SelectObject(hdc, oldPen); //Put the previous pen back instead of leaving ours selected.
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC Memhdc;
        HDC hdc;
        HBITMAP Membitmap;

        int win_width = WBounds.right - WBounds.left;
        int win_height = WBounds.bottom - WBounds.top; //Was "bottom + left".

        hdc = BeginPaint(hwnd, &ps);
        Memhdc = CreateCompatibleDC(hdc);
        Membitmap = CreateCompatibleBitmap(hdc, win_width, win_height);
        HGDIOBJ oldBitmap = SelectObject(Memhdc, Membitmap);
        FillRect(Memhdc, &WBounds, (HBRUSH)GetStockObject(WHITE_BRUSH)); //WHITE_BRUSH is a stock object id, not an HBRUSH.

        view_matrix_t vm = RPM<view_matrix_t>(moduleBase + dwViewMatrix);
        int localteam = RPM<int>(RPM<DWORD>(moduleBase + dwEntityList) + m_iTeamNum);

        for (int i = 1; i < 64; i++) {
            uintptr_t pEnt = RPM<DWORD>(moduleBase + dwEntityList + (i * 0x10));
            if (!pEnt) continue; //Empty slots used to be dereferenced anyway.
            int team = RPM<int>(pEnt + m_iTeamNum);

            if (team != localteam) {
                int health = RPM<int>(pEnt + m_iHealth);
                Vector3 pos = RPM<Vector3>(pEnt + m_vecOrigin);
                Vector3 head; head.x = pos.x; head.y = pos.y; head.z = pos.z + 72.f;
                Vector3 screenpos = WorldToScreen(pos, vm);
                Vector3 screenhead = WorldToScreen(head, vm);

                if (screenpos.z >= 0.01f && health > 0 && health < 101) {
                    Draw(Memhdc, screenpos, screenhead);
                }
            }
        }
        BitBlt(hdc, 0, 0, win_width, win_height, Memhdc, 0, 0, SRCCOPY);
        SelectObject(Memhdc, oldBitmap);
        DeleteObject(Membitmap);
        DeleteDC(Memhdc); //Only the memory DC is deleted, the one from BeginPaint belongs to EndPaint.
        EndPaint(hwnd, &ps);
        ValidateRect(hwnd, &WBounds);
        break; //Without this the case fell through into WM_ERASEBKGND.
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

DWORD WINAPI WorkLoop(LPVOID) { //Real thread signature, the old one was cast to LPTHREAD_START_ROUTINE.
    while (true) {
        InvalidateRect(EspHWND, &WBounds, true);
        Sleep(16); //16 ms * 60 fps ~ 1000 ms
    }
    return 0;
}

int main() {
    HWND GameHWND = FindWindowA(0, "Counter-Strike: Global Offensive");
    if (!GameHWND) {
        std::cout << "Game window not found." << std::endl;
        return 1;
    }

    GetClientRect(GameHWND, &WBounds);
    DWORD dwPid; GetWindowThreadProcessId(GameHWND, &dwPid);
    TargetProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwPid);
    if (!TargetProcess) {
        std::cout << "OpenProcess failed, error " << GetLastError() << std::endl;
        return 1;
    }

    moduleBase = GetModuleBaseAddress(dwPid, "client_panorama.dll");
    if (!moduleBase) {
        std::cout << "client_panorama.dll not found in the target process." << std::endl;
        CloseHandle(TargetProcess);
        return 1;
    }

    BoxPen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    HINSTANCE Hinstance = GetModuleHandleA(NULL); //The class and the window have to use the same instance handle.

    WNDCLASSEX WClass;
    MSG Msg;
    WClass.cbSize = sizeof(WNDCLASSEX);
    WClass.style = 0;
    WClass.lpfnWndProc = WndProc;
    WClass.cbClsExtra = 0;
    WClass.cbWndExtra = 0;
    WClass.hInstance = Hinstance;
    WClass.hIcon = NULL;
    WClass.hCursor = NULL;
    WClass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH); //WHITE_BRUSH is a stock object id, not an HBRUSH.
    WClass.lpszMenuName = NULL;
    WClass.lpszClassName = "CSGO-GDI-ESP";
    WClass.hIconSm = NULL;
    if (!RegisterClassExA(&WClass)) {
        std::cout << "RegisterClassEx failed, error " << GetLastError() << std::endl;
        CloseHandle(TargetProcess);
        return 1;
    }

    EspHWND = CreateWindowExA(WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_LAYERED, "CSGO-GDI-ESP", "", WS_POPUP,
        WBounds.left, WBounds.top, WBounds.right - WBounds.left, WBounds.bottom - WBounds.top,
        NULL, NULL, Hinstance, NULL);
    if (!EspHWND) {
        std::cout << "CreateWindowEx failed, error " << GetLastError() << std::endl;
        CloseHandle(TargetProcess);
        return 1;
    }

    SetLayeredWindowAttributes(EspHWND, RGB(255, 255, 255), 255, LWA_COLORKEY);
    ShowWindow(EspHWND, SW_SHOW);

    HANDLE hThread = CreateThread(NULL, 0, WorkLoop, NULL, 0, NULL);
    if (!hThread) {
        std::cout << "CreateThread failed, error " << GetLastError() << std::endl;
        CloseHandle(TargetProcess);
        return 1;
    }

    while (GetMessageA(&Msg, NULL, 0, 0) > 0) { //GetMessage already blocks, the Sleep in here did nothing.
        TranslateMessage(&Msg);
        DispatchMessageA(&Msg);
    }

    CloseHandle(hThread);
    DeleteObject(BoxPen);
    CloseHandle(TargetProcess);
    return 0;
}
