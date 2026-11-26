// SPDX-License-Identifier: GPL-3.0-only
#include "guard.h"
#include "engine.h"
#include "video.h"
#include <ctime>

static SRWLOCK lock = SRWLOCK_INIT;
static std::wstring notice;
static ULONGLONG notice_until;
void guard_say(const std::wstring &s)
{
    AcquireSRWLockExclusive(&lock);
    notice = s;
    notice_until = GetTickCount64() + 8000;
    ReleaseSRWLockExclusive(&lock);
}
std::wstring guard_notice()
{
    AcquireSRWLockShared(&lock);
    std::wstring s = GetTickCount64() < notice_until ? notice : std::wstring();
    ReleaseSRWLockShared(&lock);
    return s;
}
// Good picture first, then whatever runs almost everywhere.
int guard_next_model(int model)
{
    static const int order[] = {9, 12, 5, 0, 18};
    bool after = false, known = false;
    for (int m : order) known = known || m == model;
    for (int m : order)
    {
        if (m == model) { after = true; continue; }
        if (known && !after) continue;
        const ModelInfo *i = model_by_id(m);
        if (i->nvidia && !nvidia_rtx()) continue;
        if (m == 9 && !vfg_installed()) continue;
        return m;
    }
    return -1;
}

// ---- in the program ----
static bool recovered;
bool guard_recovered() { return recovered; }
void guard_child_begin(const wchar_t *cmd)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ini_set(L"clean_exit", 0);
    const wchar_t *r = wcsstr(cmd, L"--recovered ");
    if (!r) return;
    recovered = true;
    int from = -1, to = -1;
    swscanf(r + 12, L"%d:%d", &from, &to);
    logf("guard: started again, model %d -> %d", from, to);
    wchar_t t[200];
    if (to < 0) swprintf(t, 200, tr(L"문제가 계속되어 프레임 생성을 껐어요", L"Frame generation was turned off after repeated problems"));
    else if (from != to) swprintf(t, 200, tr(L"%ls이(가) 계속 멈춰서 %ls(으)로 바꿨어요", L"%ls kept stopping; switched to %ls"), model_by_id(from)->name, model_by_id(to)->name);
    else swprintf(t, 200, tr(L"생성이 멈춰서 다시 켰어요 (%ls)", L"Generation stopped and was restarted (%ls)"), model_by_id(to)->name);
    guard_say(t);
}
void guard_clean_exit() { ini_set(L"clean_exit", 1); ini_set(L"nr_trying", 0); }
void guard_restart(const char *why)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) Sleep(INFINITE);
    logf("guard: restart, %s", why);
    if (GetEnvironmentVariableW(L"PKAPP_NO_GUARD", nullptr, 0)) { InterlockedExchange(&once, 0); return; }
    TerminateProcess(GetCurrentProcess(), GUARD_EXIT); // threads may be stuck in a driver: nothing is cleaned up one by one
    Sleep(INFINITE);
}
void guard_again()
{
    logf("guard: started again on request");
    TerminateProcess(GetCurrentProcess(), GUARD_AGAIN);
}
void guard_fault(int model, bool &fail)
{
    static int at = -1, only = -1, n;
    static wchar_t kind[16];
    if (at == -1)
    {
        wchar_t v[64] = L"";
        at = 0;
        if (GetEnvironmentVariableW(L"PKAPP_FAULT", v, 64)) swscanf(v, L"%15l[a-z]@%d:%d", kind, &at, &only);
    }
    if (!at || (only >= 0 && model != only) || ++n < at) return;
    if (!wcscmp(kind, L"err")) fail = true;
    if (!wcscmp(kind, L"once")) fail = n < at + 3;
    if (!wcscmp(kind, L"hang")) { logf("fault: hang"); Sleep(INFINITE); }
    if (!wcscmp(kind, L"crash")) { logf("fault: crash"); *(volatile int *)16 = 1; }
    if (!wcscmp(kind, L"heap")) { logf("fault: fail fast"); __fastfail(5); }
    if (!wcscmp(kind, L"lost")) { logf("fault: device lost"); video_test_lost(); n = -1000000; }
}

// ---- the guard ----
int guard_main(const wchar_t *cmd)
{
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    std::wstring args = cmd, extra;
    time_t starts[16] = {};
    int count = 0, same = 0, last_model = -2;
    time_t last_time = 0;
    HANDLE job = CreateJobObjectW(nullptr, nullptr); // the program ends together with the guard
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK; // not what the program opens (a download, an editor)
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    auto first_only = [&] { // settings on the command line are only for the first start
        for (const wchar_t *k : {L"--model ", L"--on", L"--off", L"--bench-full", L"--bench", L"--full"})
        {
            size_t p = args.find(k);
            if (p == std::wstring::npos) continue;
            size_t e = p + wcslen(k);
            if (args[e - 1] == L' ') while (e < args.size() && args[e] != L' ') e++;
            args.erase(p, e - p);
        }
    };
    for (;;)
    {
        std::wstring line = L"\"" + std::wstring(exe) + L"\" --child " + args + extra;
        std::vector<wchar_t> buf(line.begin(), line.end());
        buf.push_back(0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe, buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) return 1;
        AssignProcessToJobObject(job, pi.hProcess);
        AllowSetForegroundWindow(pi.dwProcessId);
        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        bool again = code == GUARD_EXIT || code == GUARD_AGAIN || code >= 0x80000003u || code == 3; // asked for it, crashed or aborted; 1 is the task manager
        if (!again || ini_int(L"clean_exit", 0)) return code == GUARD_EXIT ? 0 : (int)code;
        if (code == GUARD_AGAIN)
        {
            extra.clear();
            first_only();
            Sleep(300);
            continue;
        }
        // what next?
        time_t now = time(nullptr);
        int recent = 0, model = ini_int(L"model", -1), to = model;
        for (time_t t : starts) recent += t && now - t < 600;
        same = model == last_model && now - last_time < 120 ? same + 1 : 1;
        last_model = model;
        last_time = now;
        starts[count++ % 16] = now;
        if (recent >= 8) // without generation too: nothing more to try
        {
            std::wstring say = std::wstring(tr(L"프쏙이 계속 멈춰서 닫았어요.\n캡처카드와 그래픽 드라이버를 확인해 주세요.\n\n기록: ", L"PEUSSOK kept stopping, so it was closed.\nPlease check the capture card and the graphics driver.\n\nLog: ")) + data_dir() + L"\\app.log";
            MessageBoxW(nullptr, say.c_str(), app_name(), MB_OK | MB_ICONERROR);
            return (int)code;
        }
        if (recent >= 5) { to = -1; ini_set(L"on", 0); }
        else if (same >= 2)
        {
            to = guard_next_model(model);
            if (to < 0) ini_set(L"on", 0);
            else ini_set(L"model", to);
            same = 0;
        }
        wchar_t t[96];
        swprintf(t, 96, L" --recovered %d:%d", model, to);
        extra = t;
        first_only();
        Sleep(400); // a driver that was reset needs a moment
    }
}
