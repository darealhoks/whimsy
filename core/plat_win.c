#ifdef _WIN32
#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

/* a reparse point is the ntfs stand-in for a symlink; O_NOFOLLOW has no equivalent, so
 * PLAT_NOFOLLOW is a check before the open. it races, unlike O_NOFOLLOW */
static int reparse(const char *path)
{
	DWORD a = GetFileAttributesA(path);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* PLAT_PRIVATE is a 0600 stand-in: an explicit dacl granting only the owner. inheriting the
 * parent acl would leave the keyfile readable by the local Users group */
static const char *SDDL_OWNER_ONLY = "D:P(A;;GA;;;OW)(A;;GA;;;SY)";

int plat_open(const char *path, int flags)
{
	if ((flags & PLAT_NOFOLLOW) && reparse(path)) { errno = ELOOP; return -1; }

	DWORD access = flags & (_O_WRONLY | _O_RDWR) ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
	DWORD disp = flags & _O_CREAT ? (flags & _O_EXCL ? CREATE_NEW
	                                : flags & _O_TRUNC ? CREATE_ALWAYS : OPEN_ALWAYS)
	             : flags & _O_TRUNC ? TRUNCATE_EXISTING : OPEN_EXISTING;

	SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
	if ((flags & PLAT_PRIVATE) &&
	    !ConvertStringSecurityDescriptorToSecurityDescriptorA(SDDL_OWNER_ONLY, SDDL_REVISION_1,
	                                                          &sa.lpSecurityDescriptor, NULL))
		return errno = EACCES, -1;

	DWORD attr = FILE_ATTRIBUTE_NORMAL;
	if (flags & PLAT_NOFOLLOW) attr |= FILE_FLAG_OPEN_REPARSE_POINT;
	HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, &sa, disp, attr, NULL);
	/* CREATE_ALWAYS and OPEN_ALWAYS ignore lpSecurityAttributes when the file already
	 * exists, so the dacl goes on again afterwards or a pre-created file keeps its own */
	if (h != INVALID_HANDLE_VALUE && sa.lpSecurityDescriptor) {
		BOOL present = FALSE, dflt = FALSE;
		PACL dacl = NULL;
		if (GetSecurityDescriptorDacl(sa.lpSecurityDescriptor, &present, &dacl, &dflt) && present)
			SetSecurityInfo(h, SE_FILE_OBJECT,
			                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
			                NULL, NULL, dacl, NULL);
	}
	if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
		        : e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST
		        : e == ERROR_ACCESS_DENIED ? EACCES : EIO;
		return -1;
	}
	int fd = _open_osfhandle((intptr_t)h, _O_BINARY | (flags & _O_RDONLY ? _O_RDONLY : 0));
	if (fd < 0) { CloseHandle(h); errno = EMFILE; return -1; }
	return fd;
}

int plat_mkdir(const char *path) { return _mkdir(path); }

int plat_fsync(int fd) { return _commit(fd); }

void plat_sync_dir(const char *dir) { (void)dir; }

long long plat_pwrite(int fd, const void *buf, size_t n, long long off)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
	OVERLAPPED o = { 0 };
	o.Offset = (DWORD)off;
	o.OffsetHigh = (DWORD)(off >> 32);
	DWORD w = 0;
	if (!WriteFile(h, buf, (DWORD)n, &w, &o)) { errno = EIO; return -1; }
	return w;
}

/* plain rename() fails here when the destination exists; the store's crash safety needs
 * the replace to be one step */
int plat_rename(const char *from, const char *to)
{
	if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
	errno = EIO;
	return -1;
}

/* the windows stand-in for `st_mode & 077`: any allow-ace naming someone other than the
 * owner or SYSTEM counts as world-readable. without this the keyfile permission check in
 * store.c would silently pass on every file */
static int acl_has_other(HANDLE h)
{
	PSID owner = NULL;
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
	                    &owner, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS)
		return 1;      /* unreadable acl is not proof of privacy */
	int other = 0;
	if (!dacl) other = 1;   /* a null dacl grants everyone */
	else for (DWORD i = 0; i < dacl->AceCount && !other; i++) {
		ACCESS_ALLOWED_ACE *ace;
		if (!GetAce(dacl, i, (void **)&ace) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
			continue;
		PSID s = (PSID)&ace->SidStart;
		SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
		PSID sys = NULL;
		AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys);
		if (!EqualSid(s, owner) && !(sys && EqualSid(s, sys))) other = 1;
		if (sys) FreeSid(sys);
	}
	LocalFree(sd);
	return other;
}

int plat_is_regular_private(int fd, long long *size, int *world_readable)
{
	struct _stat64 st;
	if (_fstat64(fd, &st) < 0 || !(st.st_mode & _S_IFREG) || st.st_size < 0) return -1;
	if (size) *size = st.st_size;
	if (world_readable) {
		HANDLE h = (HANDLE)_get_osfhandle(fd);
		*world_readable = h == INVALID_HANDLE_VALUE ? 1 : acl_has_other(h);
	}
	return 0;
}
#endif

typedef int plat_win_unused;   /* -Wempty-translation-unit: this file is empty off windows */
