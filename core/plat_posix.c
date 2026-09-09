#ifndef _WIN32

#include "plat.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int plat_open(const char *path, int flags)
{
	int priv = flags & PLAT_PRIVATE;
	int o = flags & ~(PLAT_NOFOLLOW | PLAT_PRIVATE);
	if (flags & PLAT_NOFOLLOW) o |= O_NOFOLLOW;
	return open(path, o, priv ? 0600 : 0666);
}

int plat_mkdir(const char *path)
{
	return mkdir(path, 0700);
}

int plat_fsync(int fd)
{
	return fsync(fd);
}

void plat_sync_dir(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return;
	fsync(fd);
	close(fd);
}

long long plat_pwrite(int fd, const void *buf, size_t n, long long off)
{
	return pwrite(fd, buf, n, (off_t)off);
}

int plat_rename(const char *from, const char *to)
{
	return rename(from, to);
}

int plat_is_regular_private(int fd, long long *size, int *world_readable)
{
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return -1;
	*size = (long long)st.st_size;
	*world_readable = (st.st_mode & 077) != 0;
	return 0;
}

#endif

typedef int plat_posix_tu;      /* -wempty-translation-unit: this file is empty on windows */
