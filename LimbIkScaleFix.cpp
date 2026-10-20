// ============================================================================
//  Limb IK Scale Fix by thesammy58 (written with Claude, Anthropic's AI model;
//  see README.md, "How this was made")
//
//  An ASI mod for The Sims 3 (32-bit). It fixes sims' hands and feet flickering
//  between two sizes every frame when hand or foot size sliders are used.
//
//  Cause: the limb IK solver's last step reads the end bone's (hand/foot)
//  current orientation by converting its model-space matrix, which includes the
//  bone's slider scale, into a quaternion. That conversion assumes a pure
//  rotation, so with scale it returns a quaternion of the wrong length, and the
//  correction written back into the pose is not unit length either. Smooth
//  Animation's blend renormalises it on some frames and not others, so the
//  hand/foot alternates between two sizes. Full write-up: docs/TECHNICAL.md.
//
//  Fix: the one call that builds that matrix is redirected to a wrapper that
//  builds it from a copy of the bone's transform with scale (1,1,1) and a
//  normalised rotation. Nothing else in the game is changed.
//
//  Ctrl+Shift+L toggles the fix while the game window is focused (A/B testing).
//  Log: LimbIkScaleFix.log next to this file. Settings: LimbIkScaleFix.ini next
//  to this file (optional).
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define LIMBIKSCALEFIX_VERSION "1.1.0"

namespace {

// ---------------------------------------------------------------------------
// Engine types and the replacement function

// Engine bone transform (48 bytes, 16-byte aligned): translation xyzw,
// rotation quaternion xyzw, scale xyzw.
struct alignas(16) BoneTransform {
    float translation[4];
    float rotation[4];
    float scale[4];
};
static_assert(sizeof(BoneTransform) == 0x30, "must match the engine layout");

// Original transform -> 4x4 matrix helper: __thiscall (transform in ECX), one
// stack argument, callee pops it. EA 1.69.47: 0x00576C50, Steam 1.67.2: 0x00577240.
typedef void(__thiscall* BuildMatrixFn)(const BoneTransform* transform, float* outMatrix);

BuildMatrixFn g_originalBuildMatrix = nullptr;
volatile LONG g_fixEnabled = 1;

// Same ABI as the original: transform in ECX, output pointer on the stack,
// callee pops 4 bytes. __fastcall with an unused EDX parameter gives exactly
// that. The source is taken as raw bytes so the compiler can't assume anything
// about its alignment.
void __fastcall BuildRotationOnlyMatrix(const void* transform, void* /*unusedEdx*/, float* outMatrix) {
    if (!g_fixEnabled) {
        g_originalBuildMatrix(static_cast<const BoneTransform*>(transform), outMatrix);
        return;
    }

    BoneTransform rotationOnly;
    memcpy(&rotationOnly, transform, sizeof(rotationOnly));

    // The solver only wants the bone's orientation here, not its size.
    rotationOnly.scale[0] = 1.0f;
    rotationOnly.scale[1] = 1.0f;
    rotationOnly.scale[2] = 1.0f;

    // Make the matrix a pure rotation even if an earlier step left a non-unit quaternion.
    float* q = rotationOnly.rotation;
    const float lengthSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (lengthSq > 1e-12f) {
        const float inverseLength = 1.0f / sqrtf(lengthSq);
        q[0] *= inverseLength;
        q[1] *= inverseLength;
        q[2] *= inverseLength;
        q[3] *= inverseLength;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }

    g_originalBuildMatrix(&rotationOnly, outMatrix);
}

// ---------------------------------------------------------------------------
// Logging and settings

const DWORD kLogMaxBytes = 256u * 1024u; // the log starts over past this size

wchar_t g_logPath[MAX_PATH];
wchar_t g_iniPath[MAX_PATH];

void Log(const char* fmt, ...) {
    char line[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear, st.wMonth, st.wDay, st.wHour,
        st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    strcat(line, "\r\n");

    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > kLogMaxBytes) {
        CloseHandle(h);
        h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD written;
    WriteFile(h, line, (DWORD)strlen(line), &written, nullptr);
    CloseHandle(h);
}

void InitPaths(HMODULE self) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        lstrcpyW(g_logPath, L"LimbIkScaleFix.log");
        lstrcpyW(g_iniPath, L".\\LimbIkScaleFix.ini");
        return;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    lstrcpyW(g_logPath, path);
    lstrcatW(g_logPath, L"LimbIkScaleFix.log");
    lstrcpyW(g_iniPath, path);
    lstrcatW(g_iniPath, L"LimbIkScaleFix.ini");
}

// The game's executable is TS3.exe (EA App / Origin, patch 1.69) or TS3W.exe
// (Steam / disc, patch 1.67). The launcher also loads ASIs, so anything else is
// ignored.
bool IsGameProcess() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash ? slash + 1 : path;
    return lstrcmpiW(name, L"TS3.exe") == 0 || lstrcmpiW(name, L"TS3W.exe") == 0;
}

// ---------------------------------------------------------------------------
// Finding the call

struct KnownBuild {
    DWORD timestamp;
    const char* name;
    uintptr_t callSite; // address of the E8 call
};

const KnownBuild kKnownBuilds[] = {
    {0x6707155C, "EA App 1.69.47", 0x0057D709},
    {0x52DEC247, "Steam 1.67.2", 0x0057E0E9},
};

// mov ebx,[esp+??] / lea edi,[edi+edi*2] / lea edx,[esp+????] / shl edi,4 /
// push edx / lea ecx,[edi+ebx] / call ???????? / movaps xmm2,[esp+????]
// The E8 is at offset 21.
const int16_t kCallSitePattern[] = {0x8B, 0x5C, 0x24, -1, 0x8D, 0x3C, 0x7F, 0x8D, 0x94, 0x24, -1, -1, 0x00, 0x00, 0xC1, 0xE7,
    0x04, 0x52, 0x8D, 0x0C, 0x1F, 0xE8, -1, -1, -1, -1, 0x0F, 0x28, 0x94, 0x24, -1, -1, 0x00, 0x00};
const size_t kCallSitePatternLen = sizeof(kCallSitePattern) / sizeof(kCallSitePattern[0]);
const size_t kCallOffset = 21;

// Original helper: push ebp / mov ebp,esp / and esp,-16 / mov edx,[ebp+8] / push edx / call <builder>
const uint8_t kHelperPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x55, 0x08, 0x52, 0xE8};

bool IsReadable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* cur = static_cast<const uint8_t*>(p);
    const uint8_t* end = cur + n;
    while (cur < end) {
        if (!VirtualQuery(cur, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD readable =
            PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD)) return false;
        cur = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

bool MatchesAt(const uint8_t* p) {
    for (size_t i = 0; i < kCallSitePatternLen; ++i) {
        if (kCallSitePattern[i] >= 0 && p[i] != (uint8_t)kCallSitePattern[i]) return false;
    }
    return true;
}

bool GetTextSection(uint8_t** start, size_t* size, DWORD* timestamp) {
    uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base || !IsReadable(base, 0x1000)) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *timestamp = nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            *start = base + sec[i].VirtualAddress;
            *size = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Returns the address of the E8, or 0 if the code isn't there (yet).
uintptr_t FindCallSite(uint8_t* text, size_t textSize, DWORD timestamp, const char** how) {
    for (const KnownBuild& b : kKnownBuilds) {
        if (b.timestamp != timestamp) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(b.callSite - kCallOffset);
        if (IsReadable(p, kCallSitePatternLen) && MatchesAt(p)) {
            *how = b.name;
            return b.callSite;
        }
    }
    if (!IsReadable(text, textSize)) return 0;
    uintptr_t found = 0;
    for (size_t i = 0; i + kCallSitePatternLen <= textSize; ++i) {
        if (text[i] != 0x8B || text[i + 1] != 0x5C) continue;
        if (MatchesAt(text + i)) {
            if (found) return 0; // more than one match: refuse to guess
            found = reinterpret_cast<uintptr_t>(text + i + kCallOffset);
        }
    }
    *how = "pattern scan";
    return found;
}

bool InstallAt(uintptr_t callSite) {
    int32_t rel;
    memcpy(&rel, reinterpret_cast<void*>(callSite + 1), 4);
    const uintptr_t target = callSite + 5 + rel;
    if (!IsReadable(reinterpret_cast<void*>(target), sizeof(kHelperPrologue)) ||
        memcmp(reinterpret_cast<void*>(target), kHelperPrologue, sizeof(kHelperPrologue)) != 0) {
        Log("Call at 0x%08X targets 0x%08X, which is not the expected matrix helper. Another mod may have hooked it; not patching.",
            (unsigned)callSite, (unsigned)target);
        return false;
    }
    g_originalBuildMatrix = reinterpret_cast<BuildMatrixFn>(target);

    const int32_t newRel = (int32_t)(reinterpret_cast<uintptr_t>(&BuildRotationOnlyMatrix) - (callSite + 5));
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(callSite + 1), 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("VirtualProtect failed (%lu); not patching.", GetLastError());
        return false;
    }
    memcpy(reinterpret_cast<void*>(callSite + 1), &newRel, 4);
    DWORD ignored;
    VirtualProtect(reinterpret_cast<void*>(callSite + 1), 4, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(callSite), 5);
    Log("Patched call at 0x%08X (original helper 0x%08X -> 0x%08X).", (unsigned)callSite, (unsigned)target,
        (unsigned)reinterpret_cast<uintptr_t>(&BuildRotationOnlyMatrix));
    return true;
}

bool GameWindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI Worker(LPVOID) {
    const int startEnabled = GetPrivateProfileIntW(L"LimbIkScaleFix", L"Enabled", 1, g_iniPath);
    const int hotkey = GetPrivateProfileIntW(L"LimbIkScaleFix", L"ToggleHotkey", 1, g_iniPath);
    InterlockedExchange(&g_fixEnabled, startEnabled ? 1 : 0);

    uint8_t* text = nullptr;
    size_t textSize = 0;
    DWORD timestamp = 0;
    if (!GetTextSection(&text, &textSize, &timestamp)) {
        Log("Could not read the game's headers; not patching.");
        return 0;
    }
    Log("LimbIkScaleFix " LIMBIKSCALEFIX_VERSION " loaded. Game build timestamp 0x%08lX. Fix starts %s.", timestamp,
        startEnabled ? "ON" : "OFF");

    // The EA App build's code is decrypted after launch, so keep looking for up to 3 minutes.
    uintptr_t callSite = 0;
    const char* how = "";
    const DWORD startTick = GetTickCount();
    while (!(callSite = FindCallSite(text, textSize, timestamp, &how))) {
        if (GetTickCount() - startTick > 180000) {
            Log("Could not find the limb IK call after 3 minutes; not patching. Unknown or modified game build?");
            return 0;
        }
        Sleep(250);
    }
    Log("Found the limb IK end-bone matrix call via %s.", how);
    if (!InstallAt(callSite)) return 0;

    if (!hotkey) return 0;
    Log("Ctrl+Shift+L toggles the fix while the game window is focused.");
    bool wasDown = false;
    for (;;) {
        Sleep(50);
        const bool down =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('L') & 0x8000);
        if (down && !wasDown && GameWindowFocused()) {
            const LONG now = InterlockedXor(&g_fixEnabled, 1) ^ 1;
            Log("Fix toggled %s.", now ? "ON" : "OFF");
            MessageBeep(now ? MB_OK : MB_ICONHAND);
        }
        wasDown = down;
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (!IsGameProcess()) return TRUE; // the launcher loads ASIs too; stay idle there
        InitPaths(instance);
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
