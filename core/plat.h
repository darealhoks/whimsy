#ifndef WHIMSY_PLAT_H
#define WHIMSY_PLAT_H
/* the posix file surface core/ uses, reimplemented for windows. callers use plat_* only. */

#include <stddef.h>
#include <stdint.h>

/* O_RDONLY/O_WRONLY/O_CREAT/O_EXCL/O_TRUNC keep their platform values; plat_open takes them
 * plus these two, which have no windows equivalent and are enforced by plat_open itself */
#define PLAT_NOFOLLOW 0x01000000        /* fail on a symlink or ntfs reparse point */
#define PLAT_PRIVATE  0x02000000        /* 0600 / owner-only acl */

int plat_open(const char *path, int flags);
int plat_mkdir(const char *path);       /* 0700; 0 or -1 with errno EEXIST preserved */
int plat_fsync(int fd);
void plat_sync_dir(const char *dir);    /* posix: fsync the dir entry. windows: no-op */
long long plat_pwrite(int fd, const void *buf, size_t n, long long off);
int plat_rename(const char *from, const char *to);      /* replaces an existing dest, atomically */
int plat_is_regular_private(int fd, long long *size, int *world_readable);

#endif
