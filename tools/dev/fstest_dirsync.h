/* For tests/test_fs_journal.py: fstest built with this header included first in every source
 * (clang -include, so runtime/linux/win32.c's write-through moves as well as hle_fs.c), where
 * SSBU_FS_DIRSYNC=einval or =eio makes every flush of a folder - fsync, and F_FULLFSYNC on macOS -
 * fail with that error: a file system that cannot flush folders, or a disk failing. Files flush
 * as usual. POSIX only (Windows flushes no folder). */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int fstest_dirsync_error(int fd) {
    const char *s = getenv("SSBU_FS_DIRSYNC");
    struct stat st;
    if (!s || fstat(fd, &st) || !S_ISDIR(st.st_mode)) return 0;
    return !strcmp(s, "eio") ? EIO : !strcmp(s, "einval") ? EINVAL : 0;
}
static inline int fstest_fsync(int fd) {
    int e = fstest_dirsync_error(fd);
    if (e) { errno = e; return -1; }
    return fsync(fd);
}
static inline int fstest_fcntl(int fd, int cmd, ...) {
    va_list a;
    va_start(a, cmd);
    long arg = va_arg(a, long);   /* (every fcntl the runtime makes passes one argument or none) */
    va_end(a);
#ifdef F_FULLFSYNC
    if (cmd == F_FULLFSYNC) {
        int e = fstest_dirsync_error(fd);
        if (e) { errno = e; return -1; }
    }
#endif
    return fcntl(fd, cmd, arg);
}
#define fsync fstest_fsync
#define fcntl fstest_fcntl
