// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>
#include <direct.h>
#include <ctype.h>

#include "../platform.h"

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH: the Vulkan renderer's vertex / index cache re-copies only pages the
// CPU wrote, runtime/nvn/vulkan/exec.c). Without it (refused): no tracking, the renderer reads guest memory in place.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);  // (more than fit: caller takes all)
    return (int)n;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// ---- process
// Release builds are GUI-subsystem executables (no console window): stdout / stderr go to lasrecomp.log next to the
// exe, and a fatal error shows a message box (plat_fatal_notice) instead of vanishing. Processes started by this one
// (the start screen re-executing itself for the game) append to the same log.
static char g_log_path[MAX_PATH];

static void log_to_file_without_console(void) {
    if (GetStdHandle(STD_ERROR_HANDLE) && GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE) return;  // console / pipe
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_log_path, sizeof g_log_path, "%slasrecomp.log", exe);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "ab" : "wb", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv("LASRECOMP_LOG_APPEND=1");
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2), or on the player's machine for
// x86-64-v2 when the CPU is older (gamelib_old_cpu): say so instead of crashing with an illegal instruction on a CPU
// without even that.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt")) return;
    MessageBoxA(NULL, "ZA Recomp needs a CPU with SSE4.2 (Intel Core i3/i5/i7 / 2008 or newer, AMD Bulldozer / 2011 or "
                "newer).", "ZA Recomp", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box, when there is no console.
static void run_end(void);
void plat_fatal_notice(void) {
    run_end();
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1024] = "ZA Recomp stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "ZA Recomp stopped because of an error:\n\n%.600s", line);
            if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nZA Recomp needs a graphics driver with Vulkan 1.3: install the "
                         "latest driver for your GPU (NVIDIA, AMD or Intel).");
            }
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    MessageBoxA(NULL, text, "ZA Recomp", MB_ICONERROR | MB_OK);
}

// (profiling) LARECOMP_SAMPLE=<delay s>,<duration s>: after the delay, a thread samples the instruction pointer of
// every other thread of the process each ~1 ms (suspend, read the context, resume) for the duration, then writes
// build/samples.txt: "<thread id> <rip> <count>" lines (map the addresses with tools/za/samples.py and nm).
// LARECOMP_SAMPLE_CALLER=<lo hex>,<hi hex>: samples inside that range (a leaf function such as memcmp) also count the
// return address on top of the stack, under thread id | 0x80000000 - who calls it.
#include <tlhelp32.h>
#define SAMP_TAB (1u << 20)
static struct { uint64_t rip; uint32_t tid, n; } *samp;
static void samp_add(uint64_t rip, uint32_t tid) {
    uint64_t k = (rip * 0x9E3779B97F4A7C15ull) ^ tid;
    uint32_t slot = (uint32_t)(k >> 40) & (SAMP_TAB - 1);
    for (int probe = 0; probe < 64; probe++, slot = (slot + 1) & (SAMP_TAB - 1)) {
        if (!samp[slot].n) { samp[slot].rip = rip; samp[slot].tid = tid; samp[slot].n = 1; return; }
        if (samp[slot].rip == rip && samp[slot].tid == tid) { samp[slot].n++; return; }
    }
}
static DWORD WINAPI sampler_main(void *arg) {
    double delay = 0, dur = 10;
    sscanf((const char *)arg, "%lf,%lf", &delay, &dur);
    Sleep((DWORD)(delay * 1000));
    samp = calloc(SAMP_TAB, sizeof *samp);
    unsigned long long caller_lo = 0, caller_hi = 0;
    if (getenv("LARECOMP_SAMPLE_CALLER")) sscanf(getenv("LARECOMP_SAMPLE_CALLER"), "%llx,%llx", &caller_lo, &caller_hi);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(dur * 1000);
    uint64_t total = 0;
    while (GetTickCount64() < end) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {.dwSize = sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL};
            if (SuspendThread(h) != (DWORD)-1) {
                if (GetThreadContext(h, &ctx)) {
                    samp_add(ctx.Rip, te.th32ThreadID);
                    if (ctx.Rip >= caller_lo && ctx.Rip < caller_hi)  // (the thread is suspended: its stack can be read)
                        samp_add(*(const uint64_t *)(uintptr_t)ctx.Rsp, te.th32ThreadID | 0x80000000u);
                    total++;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        Sleep(1);
    }
    FILE *f = fopen("build/samples.txt", "w");
    if (f) {
        for (uint32_t i = 0; i < SAMP_TAB; i++)
            if (samp[i].n) fprintf(f, "%u %llx %u\n", samp[i].tid, (unsigned long long)samp[i].rip, samp[i].n);
        fclose(f);
    }
    HMODULE game = GetModuleHandleA("lasgame.dll"), exe = GetModuleHandleA(NULL);
    fprintf(stderr, "[sample] %llu samples -> build/samples.txt (lasgame.dll at %p, exe at %p)\n", (unsigned long long)total,
            (void *)game, (void *)exe);
    return 0;
}

void plat_init(void) {
    log_to_file_without_console();
    if (getenv("LARECOMP_SAMPLE")) CreateThread(NULL, 0, sampler_main, getenv("LARECOMP_SAMPLE"), 0, NULL);
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// RtlGetVersion: the real version (GetVersionEx answers what the manifest declares). Build 22000+ is Windows 11.
void plat_os_name(char *buf, size_t size) {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn f = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (!f || f(&v) != 0) { snprintf(buf, size, "Windows"); return; }
    snprintf(buf, size, "Windows %s %lu.%lu.%lu", v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "11" : "10",
             v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    run_end();  // (an end the log tells; the new process marks its own run)
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx lasrecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    run_end();  // (the log has the report: not an end without a word)
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- a stuck game, a silent end (#38: play.log just stopped - frozen and closed from the Task Manager, or ended by
// Windows without our crash report: heap corruption, a fail-fast)
// plat_dump_threads (runtime/nvn/vulkan/device.c hang_watch: no frame for a while): every other thread's place, into
// the log - its instruction and the code addresses on its stack (a scan, not an unwind: the stack is read with
// ReadProcessMemory, which can't fault, and nothing that could take a lock runs while a thread is suspended).
// Frames in the game code are named by their guest function (rt_host_fn_name).
void plat_dump_threads(void) {
    HMODULE exe = GetModuleHandleA(NULL), game = GetModuleHandleA("lasgame.dll");
    typedef HRESULT(WINAPI * GetDescFn)(HANDLE, PWSTR *);
    GetDescFn get_desc = (GetDescFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    static uint64_t stack[4096];  // (32 KB of each stack: the frames nearest the top)
    int threads = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        CONTEXT ctx = {.ContextFlags = CONTEXT_CONTROL};
        size_t words = 0;
        bool got = false;
        if (SuspendThread(h) != (DWORD)-1) {
            if ((got = GetThreadContext(h, &ctx)))  // (a page at a time, up to the first that can't be read: the top)
                for (uint64_t a = ctx.Rsp & ~7ull; words < 4096;) {
                    size_t chunk = (size_t)((4096 - (a & 4095)) / 8);
                    if (chunk > 4096 - words) chunk = 4096 - words;
                    SIZE_T n = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, stack + words, chunk * 8, &n) ||
                        n != chunk * 8)
                        break;
                    words += chunk, a += chunk * 8;
                }
            ResumeThread(h);
        }
        char name[64] = "";
        PWSTR desc = NULL;
        if (get_desc && SUCCEEDED(get_desc(h, &desc)) && desc) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, sizeof name, NULL, NULL);
            LocalFree(desc);
        }
        CloseHandle(h);
        if (!got) continue;
        threads++;
        char line[1024];
        int len = snprintf(line, sizeof line, "[hang] thread %lu%s%s%s:", te.th32ThreadID, *name ? " (" : "", name,
                           *name ? ")" : "");
        int shown = 0;
        for (size_t i = 0; i <= words && shown < 10 && len < (int)sizeof line - 96; i++) {
            uint64_t pc = i == 0 ? ctx.Rip : stack[i - 1];
            if (pc >= g_lo && pc < g_hi) continue;  // (guest memory: never code, and VirtualQuery is slow on it)
            HMODULE m = NULL;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)(uintptr_t)pc, &m))
                continue;
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)(uintptr_t)pc, &mi, sizeof mi) ||
                !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                continue;
            uint64_t off = 0;
            const char *s = m == exe || m == game ? rt_host_fn_name(pc, &off) : plat_symbol(pc, &off);
            if (s) len += snprintf(line + len, sizeof line - len, " %s+%llx", s, (unsigned long long)off);
            else len += snprintf(line + len, sizeof line - len, " %s+%llx", m == game ? "lasgame" : "lasrecomp",
                                 (unsigned long long)(pc - (uint64_t)(uintptr_t)m));
            shown++;
        }
        fprintf(stderr, "%s\n", line);
    }
    CloseHandle(snap);
    fprintf(stderr, "[hang] %d threads\n", threads);
    fflush(stderr);
}

// The last run's end: cache/running holds a run's process id from its start (plat_run_begin) until it ends in a way
// the log tells (a closed window, back to the start screen, our crash report). Still there at the next start: that
// run ended without a word, and what Windows' event log recorded about it (Application Error / Hang: the exception and
// the module) goes into this log.
static void run_end(void) { remove("cache/running"); }

static void last_run_report(unsigned long pid) {
    typedef HANDLE(WINAPI * QueryFn)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
    typedef BOOL(WINAPI * NextFn)(HANDLE, DWORD, HANDLE *, DWORD, DWORD, DWORD *);
    typedef BOOL(WINAPI * RenderFn)(HANDLE, HANDLE, DWORD, DWORD, PVOID, DWORD *, DWORD *);
    typedef BOOL(WINAPI * CloseFn)(HANDLE);
    HMODULE w = LoadLibraryA("wevtapi.dll");
    QueryFn query = w ? (QueryFn)(void *)GetProcAddress(w, "EvtQuery") : NULL;
    NextFn next = w ? (NextFn)(void *)GetProcAddress(w, "EvtNext") : NULL;
    RenderFn render = w ? (RenderFn)(void *)GetProcAddress(w, "EvtRender") : NULL;
    CloseFn evt_close = w ? (CloseFn)(void *)GetProcAddress(w, "EvtClose") : NULL;
    bool found = false;
    HANDLE q = query && next && render && evt_close
        ? query(NULL, L"Application",
                L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 604800000]]]",
                0x1 | 0x200)  // (EvtQueryChannelPath | EvtQueryReverseDirection: the newest first)
        : NULL;
    static wchar_t xml[16384];
    static char s[32768];
    char want[32];
    snprintf(want, sizeof want, ">0x%lx<", pid);  // (<Data Name='ProcessId'>0x1a2b</Data>)
    HANDLE ev;
    DWORD got;
    for (int seen = 0; q && !found && seen < 200 && next(q, 1, &ev, 1000, 0, &got) && got; seen++) {
        DWORD used = 0, props = 0;
        if (render(NULL, ev, 1 /* EvtRenderEventXml */, sizeof xml - 2, xml, &used, &props)) {
            WideCharToMultiByte(CP_UTF8, 0, xml, -1, s, sizeof s, NULL, NULL);
            for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (strstr(s, "lasrecomp") && strstr(s, want)) {
                found = true;
                char out[1024];
                int n = snprintf(out, sizeof out, "[system] the last run (process %lu) ended without a report; Windows "
                                 "recorded %s", pid, strstr(s, "<eventid>1002<") ? "it not responding (a hang)" : "a crash");
                static const char *const keys[] = {"modulename", "exceptioncode", "faultingoffset"};
                for (int k = 0; k < 3; k++) {
                    char tag[48];
                    snprintf(tag, sizeof tag, "name='%s'>", keys[k]);
                    const char *v = strstr(s, tag);
                    if (!v) continue;
                    v += strlen(tag);
                    n += snprintf(out + n, sizeof out - n, ", %s %.*s", keys[k], (int)strcspn(v, "<"), v);
                }
                fprintf(stderr, "%s\n", out);
            }
        }
        evt_close(ev);
    }
    if (q) evt_close(q);
    if (!found)
        fprintf(stderr, "[system] the last run (process %lu) ended without a report: closed from the Task Manager, a "
                        "power loss, or ended by Windows (nothing on it in Windows' event log)\n", pid);
}

void plat_run_begin(void) {
    _mkdir("cache");
    FILE *f = fopen("cache/running", "rb");
    unsigned long last = 0;
    if (f) {
        if (fscanf(f, "%lu", &last) != 1) last = 0;
        fclose(f);
        if (last) last_run_report(last);
    }
    if ((f = fopen("cache/running", "wb"))) fprintf(f, "%lu\n", GetCurrentProcessId()), fclose(f);
    atexit(run_end);
}

// ---- POSIX shims (win_compat.h)
// Guest pages are committed on the CPU's first touch (vectored_handler), but ReadFile / WriteFile never fault:
// into a page that is only reserved the call fails (ERROR_NOACCESS) instead of committing it. So the range is
// committed first, and a failure is logged rather than handed back as a silent short read.
static void commit_guest_range(uint64_t a, uint64_t n) {
    if (!n || a + n <= g_lo || a >= g_hi) return;
    uint64_t end = a + n < g_hi ? a + n : g_hi;
    if (a < g_lo) a = g_lo;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return;
        uint64_t rend = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_RESERVE) {
            uint64_t lo = a & ~(COMMIT_CHUNK - 1), hi = ((rend < end ? rend : end) + COMMIT_CHUNK - 1) & ~(COMMIT_CHUNK - 1);
            if (hi > rend) hi = rend;
            if (!VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_COMMIT, PAGE_READWRITE))
                VirtualAlloc((void *)(uintptr_t)(a & ~0xFFFull), ((rend < end ? rend : end) - (a & ~0xFFFull) + 0xFFF) & ~0xFFFull,
                             MEM_COMMIT, PAGE_READWRITE);
        }
        a = rend;
    }
}
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    commit_guest_range((uint64_t)(uintptr_t)buf, n);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            static int nlog;
            if (nlog++ < 40)
                fprintf(stderr, "[plat] %s of %#llx bytes at file offset %#llx into %p failed after %#llx: error %lu\n",
                        write ? "write" : "read", (unsigned long long)n, (unsigned long long)off, buf, (unsigned long long)done, err);
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // every child in one job that ends with this process (a compiler's own children too: no leftovers on quit)
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)pi.hProcess;
}

void plat_proc_kill(PlatProc p) {  // (what it started ends with this process: the job above)
    TerminateProcess((HANDLE)p, 255);
    WaitForSingleObject((HANDLE)p, INFINITE);
    CloseHandle((HANDLE)p);
}

int plat_proc_wait(PlatProc p, bool block) {
    HANDLE h = (HANDLE)p;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    if (w != WAIT_OBJECT_0) return 255;  // (not a process we can wait for: ended as far as the caller is concerned)
    DWORD code = 255;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) {  // (exit()'s last handler, runtime/nvn/vulkan/device.c: the others registered after
    run_end();                 // ours never run - so the run's end is marked here)
    TerminateProcess(GetCurrentProcess(), 0);
}

// stderr still the console (no redirect: started by a double click) - main.c then logs to a file
int rt_stderr_is_console(void) {
    DWORD t = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    return t == FILE_TYPE_CHAR || t == FILE_TYPE_UNKNOWN;
}

static void set_errno_win(DWORD e) {
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST
          : e == ERROR_DIR_NOT_EMPTY ? ENOTEMPTY : EACCES;
}

int plat_open_file(const char *path, int flags) {
    DWORD access = (flags & O_RDWR) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD disp = !(flags & O_CREAT) ? OPEN_EXISTING : (flags & O_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { set_errno_win(GetLastError()); return -1; }
    int fd = _open_osfhandle((intptr_t)h, ((flags & O_RDWR) ? _O_RDWR : _O_RDONLY) | _O_BINARY);
    if (fd < 0) CloseHandle(h);
    return fd;
}

int plat_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, 0)) return 0;  // (no MOVEFILE_REPLACE_EXISTING: the console's rename refuses too)
    set_errno_win(GetLastError());
    return -1;
}

// A hash of what the screen shows in the window's client area (32x18 samples of the composed desktop), while it is the
// foreground window: runtime/nvn/vulkan/device.c screen_watch tells a window that stopped showing the game's frames.
bool plat_window_screen_hash(void *hwnd, uint32_t *out) {
    HWND w = (HWND)hwnd;
    if (!w || GetForegroundWindow() != w || IsIconic(w)) return false;
    RECT rc;
    if (!GetClientRect(w, &rc) || rc.right < 64 || rc.bottom < 36) return false;
    POINT o = {0, 0};
    ClientToScreen(w, &o);
    HDC sdc = GetDC(NULL);
    if (!sdc) return false;
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = 32, .biHeight = -18, .biPlanes = 1,
                                   .biBitCount = 32, .biCompression = BI_RGB}};
    void *bits = NULL;
    HBITMAP bm = mdc ? CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    bool ok = false;
    if (bm) {
        HGDIOBJ old = SelectObject(mdc, bm);
        SetStretchBltMode(mdc, COLORONCOLOR);
        ok = StretchBlt(mdc, 0, 0, 32, 18, sdc, o.x, o.y, rc.right, rc.bottom, SRCCOPY) != 0;
        GdiFlush();
        if (ok) {
            uint32_t h = 2166136261u;
            const uint8_t *p = bits;
            for (int i = 0; i < 32 * 18 * 4; i++) h = (h ^ p[i]) * 16777619u;
            *out = h;
        }
        SelectObject(mdc, old);
        DeleteObject(bm);
    }
    if (mdc) DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    return ok;
}
