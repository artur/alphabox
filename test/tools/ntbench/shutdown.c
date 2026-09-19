/* shutdown.c — shut Windows down, the last step of an nt_bench.sh run.
 *
 * It is what flushes C:. The benchmark writes its numbers to C:\NADA\OUT.TXT
 * and the harness reads them off the disk image afterwards, so without a
 * clean shutdown the results sit in Windows' cache and never reach the image.
 * EWX_POWEROFF is asked for so that a HAL able to power the machine down ends
 * the emulator by itself; the emulated ES40 instead resets to SRM, which is
 * what nt_bench.sh watches for.
 *
 * A shutdown needs the shutdown privilege, which a logged-on administrator
 * holds but must enable first — hence the token dance.
 *
 * Adapted from nada's tests/ntppc/shutdown.c (the Windows NT for PowerPC test
 * harness), by permission and with thanks. Vendored rather than referenced so
 * that neither repository depends on a file the other's tests exercise.
 */
#define WINAPI __stdcall
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct { DWORD LowPart; int HighPart; } LUID;
typedef struct { LUID Luid; DWORD Attributes; } LUID_AND_ATTRIBUTES;
typedef struct { DWORD PrivilegeCount; LUID_AND_ATTRIBUTES Privileges[1]; } TOKEN_PRIVILEGES;

HANDLE WINAPI GetCurrentProcess(void);
BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, HANDLE *token);
BOOL WINAPI LookupPrivilegeValueA(const char *system, const char *name, LUID *luid);
BOOL WINAPI AdjustTokenPrivileges(HANDLE token, BOOL disable_all, TOKEN_PRIVILEGES *state, DWORD len, void *previous, DWORD *retlen);
BOOL WINAPI ExitWindowsEx(unsigned int flags, DWORD reserved);

#define TOKEN_ADJUST_PRIVILEGES 0x20
#define TOKEN_QUERY 0x08
#define SE_PRIVILEGE_ENABLED 2
#define EWX_SHUTDOWN 1
#define EWX_FORCE 4
#define EWX_POWEROFF 8

int main(void)
{
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        LookupPrivilegeValueA(0, "SeShutdownPrivilege", &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, 0, &tp, 0, 0, 0);
    }
    if (ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCE, 0)) return 0;
    return 1;
}
