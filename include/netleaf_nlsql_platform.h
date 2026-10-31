/**
 * @file netleaf_nlsql_platform.h
 * @brief 平台兼容层：Linux/macOS（POSIX）与 Windows（MinGW-w64 / clang）。
 *
 * 仅头文件、全 inline，不引入额外第三方依赖；扩展插件、测试与核心共用。
 * 覆盖：动态加载、文件持久化（fsync/原子替换）、数据目录锁、目录创建、
 * 套接字 I/O 与错误码、插件文件后缀。
 */
#ifndef NETLEAF_NLSQL_PLATFORM_H
#define NETLEAF_NLSQL_PLATFORM_H

#include <errno.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* WSAPoll 需要 Vista+ */
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <share.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define NLSQL_OS_WIN 1
#define NLSQL_PLUGIN_SUFFIX ".dll"
#define NLSQL_NETLEAF_LIB "netleaf.dll"

/* MinGW 的 localtime_r 受特性宏影响，统一用封装：拷贝到调用方 tm */
static inline struct tm* nlsql_localtime_r(const time_t* t, struct tm* out) {
    struct tm* r = localtime(t);
    if (!r) return NULL;
    *out = *r;
    return out;
}
#define localtime_r nlsql_localtime_r

/* CRT 对应物：fsync → _commit，getpid → _getpid，mkdir → _mkdir */
#define fsync(fd) _commit(fd)
#define getpid() _getpid()
#ifndef mkdir
#define mkdir(path, mode) _mkdir(path)
#endif

/* socket 常量/接口映射 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SIGPIPE
#define SIGPIPE 13 /* Windows 无 SIGPIPE；仅为源码里 signal(SIGPIPE, ...) 编译通过 */
#endif
#ifndef poll
#define poll(fds, n, timeout) WSAPoll((fds), (n), (timeout))
#endif

/* Windows 无 <sys/uio.h>：定义与 POSIX 同布局的最小 iovec */
struct iovec {
    void*  iov_base;
    size_t iov_len;
};

/* MinGW 无 strcasestr（GNU 扩展）：ASCII 大小写无关子串查找 */
static inline const char* nlsql_strcasestr(const char* hay, const char* needle) {
    if (!needle || !*needle) return hay;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return hay;
    }
    return NULL;
}
#define strcasestr nlsql_strcasestr

#else /* POSIX */

#define NLSQL_OS_WIN 0
#define NLSQL_PLUGIN_SUFFIX ".so"
#define NLSQL_NETLEAF_LIB "libnetleaf.so"

#include <unistd.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#endif /* _WIN32 */

/* =========================================
 * 动态加载
 * ========================================= */

#ifdef _WIN32

#ifndef RTLD_LAZY
#define RTLD_LAZY 0
#define RTLD_NOW 0
#define RTLD_GLOBAL 0
#define RTLD_LOCAL 0
#endif

static inline void* nlsql_dl_open(const char* path, int flags) {
    (void)flags;
    if (!path) return (void*)GetModuleHandleA(NULL); /* 等价 dlopen(NULL)：取宿主符号 */
    return (void*)LoadLibraryA(path);
}
static inline void* nlsql_dl_sym(void* lib, const char* name) {
    return lib ? (void*)GetProcAddress((HMODULE)lib, name) : NULL;
}
static inline const char* nlsql_dl_error(void) {
    static char buf[64];
    snprintf(buf, sizeof buf, "Win32 error %lu", (unsigned long)GetLastError());
    return buf;
}
static inline int nlsql_dl_close(void* lib) {
    return lib ? (FreeLibrary((HMODULE)lib) ? 0 : -1) : 0;
}

#else

#include <dlfcn.h>
static inline void* nlsql_dl_open(const char* path, int flags) { return dlopen(path, flags); }
static inline void* nlsql_dl_sym(void* lib, const char* name) { return dlsym(lib, name); }
static inline const char* nlsql_dl_error(void) { return dlerror(); }
static inline int nlsql_dl_close(void* lib) { return dlclose(lib); }

#endif

/* 拼插件路径：<dir>/<base><后缀>（扩展名由平台决定） */
static inline void nlsql_plugin_path(char* out, size_t cap, const char* dir, const char* base) {
    snprintf(out, cap, "%s/%s%s", dir, base, NLSQL_PLUGIN_SUFFIX);
}

/* =========================================
 * 路径
 * ========================================= */

/* 是否绝对路径：POSIX 以 '/' 开头；Windows 另接受盘符 "C:/" / "C:" */
static inline int nlsql_path_is_abs(const char* p) {
#ifdef _WIN32
    if (!p || !p[0]) return 0;
    if (p[0] == '/') return 1;
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
#else
    return p && p[0] == '/';
#endif
}

/* 就地规范化分隔符：Windows 反斜杠 → '/' */
static inline void nlsql_path_slashify(char* p) {
#ifdef _WIN32
    for (; p && *p; p++) if (*p == '\\') *p = '/';
#else
    (void)p;
#endif
}

/* =========================================
 * 文件持久化
 * ========================================= */

/* 打开普通文件（读/写）：POSIX 语义即二进制；MinGW 的 open() 默认文本模式，
   会把 '\n' 翻译成 "\r\n" 且写计数与文件位置不一致，导致 journal 头部被覆盖、
   字节级格式（magic/CRC）全错。这里统一强制 O_BINARY。 */
static inline int nlsql_file_open(const char* path, int flags, int mode) {
#ifdef _WIN32
    return open(path, flags | O_BINARY, mode);
#else
    return open(path, flags, mode);
#endif
}

/* 加密级随机字节：Windows 用 BCryptGenRandom，POSIX 读 /dev/urandom。
   返回 1 = 成功，0 = 失败（调用方可退化为混合种子）。 */
static inline int nlsql_random_bytes(void* buf, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
#endif
}

/* 原子替换：Windows 用 MoveFileEx(REPLACE_EXISTING)，POSIX 用 rename */
static inline int nlsql_rename_replace(const char* from, const char* to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* 数据目录独占锁：0 = 成功（*fd_out 有效），1 = 已被占用，-1 = 其它错误。
   Windows 用 CRT 共享模式（_SH_DENYRW）实现跨进程独占，等价 flock(LOCK_EX|LOCK_NB)。 */
static inline int nlsql_lock_exclusive(const char* path, int* fd_out) {
#ifdef _WIN32
    int fd = -1;
    errno_t e = _sopen_s(&fd, path, _O_CREAT | _O_RDWR | _O_NOINHERIT,
                         _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (e != 0) return (e == EACCES) ? 1 : -1;
#else
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return 1;
    }
#endif
    *fd_out = fd;
    return 0;
}

/* 释放锁：关闭句柄即释放（两种平台语义一致） */
static inline void nlsql_unlock_exclusive(int fd) {
    if (fd >= 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }
}

/* =========================================
 * 运行环境
 * ========================================= */

/* 可用 CPU 数（用于压实编码分块；1 表示串行） */
static inline long nlsql_cpu_count(void) {
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (long)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
#endif
}

/* 毫秒级休眠（测试与重试节拍用） */
static inline void nlsql_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* 单调毫秒计数：用于测试临时目录/日志的时间戳后缀。
   Wine 下 getpid() 在同一会话内恒定，仅靠 pid 命名会命中上次运行的残留目录。 */
static inline uint64_t nlsql_mono_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (uint64_t)tv.tv_sec * 1000u + (uint64_t)(tv.tv_usec / 1000);
    }
#endif
}

/* =========================================
 * 套接字
 * ========================================= */

#ifdef _WIN32
typedef int nlsql_socklen_t;
#else
typedef socklen_t nlsql_socklen_t;
#endif

/* 进程内一次性初始化（Windows 必须 WSAStartup；POSIX 为空操作） */
static inline int nlsql_sock_global_init(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
        done = 1;
    }
#endif
    return 0;
}
static inline void nlsql_sock_global_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static inline int nlsql_sock_close(int fd) {
#ifdef _WIN32
    return closesocket((SOCKET)fd);
#else
    return close(fd);
#endif
}

/* 套接字收发（POSIX 下 send 带 MSG_NOSIGNAL，避免 SIGPIPE 杀宿主） */
static inline ssize_t nlsql_sock_recv(int fd, void* buf, size_t n) {
#ifdef _WIN32
    return (ssize_t)recv((SOCKET)fd, (char*)buf, (int)n, 0);
#else
    return recv(fd, buf, n, 0);
#endif
}
static inline ssize_t nlsql_sock_send(int fd, const void* buf, size_t n) {
#ifdef _WIN32
    return (ssize_t)send((SOCKET)fd, (const char*)buf, (int)n, 0);
#else
    return send(fd, buf, n, MSG_NOSIGNAL);
#endif
}

/* 最近一次套接字错误码（Windows 是 WSAGetLastError，POSIX 是 errno） */
static inline int nlsql_sock_error(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
/* 是否被信号/中断打断、可安全重试 */
static inline int nlsql_sock_interrupted(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

#endif /* NETLEAF_NLSQL_PLATFORM_H */
