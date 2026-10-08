/*
 * CapsSwitch — переключение раскладки клавишей CapsLock.
 *
 *   CapsLock        -> следующая раскладка (RU <-> EN, по кругу если раскладок больше)
 *   Shift+CapsLock  -> обычный CapsLock (заглавные)
 *
 * Работает через low-level keyboard hook, без окон и без зависимостей.
 *
 * Аргументы командной строки (их вызывает установщик):
 *   --install    зарегистрировать задачу автозапуска при входе любого пользователя
 *                (с наивысшими доступными правами, чтобы работало и в окнах администратора)
 *   --uninstall  остановить все копии программы и удалить задачу автозапуска
 *   --quit       остановить все запущенные копии
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

#define TASK_NAME L"CapsSwitch"

static HHOOK g_hook;
static BOOL g_capsSwallowed; /* текущее нажатие CapsLock перехвачено -> глотаем и автоповтор, и отпускание */
static BOOL g_lshift, g_rshift;

/* ---------- переключение раскладки ---------- */

static void switch_layout(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return;

    /* Текущую раскладку берём у потока, где сейчас фокус ввода */
    DWORD fgThread = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gti;
    ZeroMemory(&gti, sizeof gti);
    gti.cbSize = sizeof gti;
    DWORD focusThread = fgThread;
    if (GetGUIThreadInfo(fgThread, &gti) && gti.hwndFocus)
        focusThread = GetWindowThreadProcessId(gti.hwndFocus, NULL);

    HKL list[64];
    int n = GetKeyboardLayoutList(64, list);
    if (n < 2)
        return;

    HKL cur = GetKeyboardLayout(focusThread);
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == cur)
            break;
    HKL next = (i < n) ? list[(i + 1) % n] : list[0];

    PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)next);
}

/* ---------- удалённый доступ и виртуальные машины ---------- */

/* Если активно окно удалённого рабочего стола или виртуальной машины,
   CapsLock отдаём ему как есть: раскладку переключит CapsSwitch на той стороне. */
static const WCHAR *const k_remote_clients[] = {
    L"mstsc.exe", L"msrdc.exe", L"msrdcw.exe", L"Windows365.exe", L"WindowsApp.exe",
    L"RDCMan.exe", L"mRemoteNG.exe", L"RoyalTS.exe", L"vmconnect.exe",
    L"AnyDesk.exe", L"TeamViewer.exe", L"rustdesk.exe", L"parsecd.exe",
    L"RemotePCDesktop.exe", L"Splashtop Business.exe", L"SRStreamer.exe",
    L"vncviewer.exe", L"tvnviewer.exe", L"VirtualBoxVM.exe", L"vmware.exe",
    L"vmware-vmx.exe", L"vmware-remotemks.exe", L"prl_client_app.exe",
    NULL
};

static BOOL foreground_is_remote_client(void)
{
    static DWORD cachedPid;
    static BOOL cachedResult;

    HWND fg = GetForegroundWindow();
    if (!fg)
        return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == cachedPid)
        return cachedResult;

    BOOL result = FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        WCHAR path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            const WCHAR *name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            for (int i = 0; k_remote_clients[i]; i++)
                if (_wcsicmp(name, k_remote_clients[i]) == 0) {
                    result = TRUE;
                    break;
                }
        }
        CloseHandle(h);
    }
    cachedPid = pid;
    cachedResult = result;
    return result;
}

/* ---------- перехват клавиатуры ---------- */

static BOOL shift_down(void)
{
    /* Своё отслеживание + системное состояние: при удалённом доступе
       события Shift иногда приходят не так, как с физической клавиатуры */
    return g_lshift || g_rshift || (GetAsyncKeyState(VK_SHIFT) & 0x8000);
}

static LRESULT CALLBACK kbd_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

        switch (k->vkCode) {
        case VK_LSHIFT: g_lshift = down; break;
        case VK_RSHIFT: g_rshift = down; break;
        case VK_SHIFT:  g_lshift = g_rshift = down; break;
        case VK_CAPITAL:
            /* Синтетические (LLKHF_INJECTED) нажатия обрабатываем так же:
               так приходят клавиши через AnyDesk, TeamViewer и т.п. */
            if (down) {
                if (g_capsSwallowed)
                    return 1; /* автоповтор удерживаемой клавиши */
                if (!shift_down() && !foreground_is_remote_client()) {
                    g_capsSwallowed = TRUE;
                    switch_layout();
                    return 1;
                }
                /* Shift+CapsLock: пропускаем дальше, Windows сама переключит CapsLock */
            } else if (g_capsSwallowed) {
                g_capsSwallowed = FALSE;
                return 1;
            }
            break;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

/* ---------- служебное ---------- */

static void kill_other_instances(void)
{
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    const WCHAR *name = wcsrchr(self, L'\\');
    name = name ? name + 1 : self;

    DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != me && _wcsicmp(pe.szExeFile, name) == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 0);
                    WaitForSingleObject(h, 3000);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static DWORD run_hidden(WCHAR *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD rc = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc;
}

static void schtasks_path(WCHAR *buf, UINT cch)
{
    UINT len = GetSystemDirectoryW(buf, cch);
    lstrcpynW(buf + len, L"\\schtasks.exe", (int)(cch - len));
}

/* Задача: при входе любого пользователя (группа Users, S-1-5-32-545),
   с наивысшими доступными правами, без лимита времени, обычный приоритет. */
static int install_task(void)
{
    WCHAR exe[MAX_PATH], tmpdir[MAX_PATH], xmlPath[MAX_PATH], st[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    GetTempPathW(MAX_PATH, tmpdir);
    GetTempFileNameW(tmpdir, L"cps", 0, xmlPath);

    static WCHAR xml[4096]; /* wsprintf ограничен 1024 символами — не подходит */
    _snwprintf(xml, sizeof xml / sizeof xml[0] - 1,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>CapsLock switches keyboard layout; Shift+CapsLock toggles Caps Lock.</Description></RegistrationInfo>\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><GroupId>S-1-5-32-545</GroupId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>4</Priority>\r\n"
        L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>\"%ls\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n",
        exe);

    HANDLE f = CreateFileW(xmlPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    DWORD w;
    WCHAR bom = 0xFEFF;
    WriteFile(f, &bom, sizeof bom, &w, NULL);
    WriteFile(f, xml, (DWORD)(lstrlenW(xml) * sizeof(WCHAR)), &w, NULL);
    CloseHandle(f);

    WCHAR cmd[2 * MAX_PATH + 64] = {0};
    schtasks_path(st, MAX_PATH);
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0] - 1, L"\"%ls\" /Create /TN \"" TASK_NAME L"\" /XML \"%ls\" /F", st, xmlPath);
    DWORD rc = run_hidden(cmd);
    DeleteFileW(xmlPath);
    return rc == 0 ? 0 : 1;
}

static int uninstall_task(void)
{
    WCHAR st[MAX_PATH], cmd[MAX_PATH + 64] = {0};
    schtasks_path(st, MAX_PATH);
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0] - 1, L"\"%ls\" /Delete /TN \"" TASK_NAME L"\" /F", st);
    run_hidden(cmd);
    return 0; /* задачи могло и не быть — это не ошибка */
}

/* ---------- точка входа ---------- */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    (void)hPrev; (void)show;

    if (wcsstr(cmd, L"--install")) {
        kill_other_instances();
        return install_task();
    }
    if (wcsstr(cmd, L"--uninstall")) {
        kill_other_instances();
        return uninstall_task();
    }
    if (wcsstr(cmd, L"--quit")) {
        kill_other_instances();
        return 0;
    }

    /* Одна копия на сеанс пользователя */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\CapsSwitch.SingleInstance");
    if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    /* Хук должен отвечать быстро, иначе Windows его отключит */
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, hInst, 0);
    if (!g_hook)
        return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    CloseHandle(mtx);
    return 0;
}
