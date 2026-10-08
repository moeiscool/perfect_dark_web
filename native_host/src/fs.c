// Files, time and memory for pd.wasm: the libc calls Emscripten leaves to its JavaScript runtime
// (__syscall_*, WASI fd_*, time zone, heap growth). The game sees one file tree:
//   /data -> --data (the ROM)   /save -> --save (Game Pak, pd.ini)   /tmp, others -> --tmp
// Values that cross into the game use Emscripten's ABI: WASI errno numbers, musl's O_* flags,
// and its struct stat / struct tm layouts (wasm32).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "host.h"

// WASI errno
enum {
	W_ESUCCESS = 0, W_EACCES = 2, W_EBADF = 8, W_EEXIST = 20, W_EINVAL = 28, W_EIO = 29,
	W_EISDIR = 31, W_EMFILE = 33, W_ENOENT = 44, W_ENOSPC = 51, W_ENOSYS = 52, W_ENOTDIR = 54,
	W_ENOTEMPTY = 55, W_ENOTTY = 59, W_EPERM = 63, W_ESPIPE = 70,
};

// musl (Emscripten) flags
#define W_O_ACCMODE   03
#define W_O_CREAT     0100
#define W_O_EXCL      0200
#define W_O_TRUNC     01000
#define W_O_APPEND    02000
#define W_O_DIRECTORY 0200000
#define W_AT_FDCWD    (-100)
#define W_AT_REMOVEDIR 0x200
#define W_F_GETFD 1
#define W_F_SETFD 2
#define W_F_GETFL 3
#define W_F_SETFL 4

static int werrno(int e)
{
	switch (e) {
	case 0: return W_ESUCCESS;
	case EACCES: return W_EACCES;
	case EBADF: return W_EBADF;
	case EEXIST: return W_EEXIST;
	case EINVAL: return W_EINVAL;
	case EISDIR: return W_EISDIR;
	case EMFILE: return W_EMFILE;
	case ENOENT: return W_ENOENT;
	case ENOSPC: return W_ENOSPC;
	case ENOTDIR: return W_ENOTDIR;
	case ENOTEMPTY: return W_ENOTEMPTY;
	case ENOTTY: return W_ENOTTY;
	case EPERM: return W_EPERM;
	case ESPIPE: return W_ESPIPE;
	default: return W_EIO;
	}
}

/* ------------------------------------------------------------------------
 * paths
 * ------------------------------------------------------------------------ */

static void mkdirs(const char *path)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

// the host path for one of the game's paths; returns 0 if it's unusable
static int hostPath(const char *gpath, char *out, size_t size)
{
	static const struct { const char *prefix; int which; } map[] = {
		{ "/data", 0 }, { "/save", 1 }, { "/tmp", 2 },
	};

	if (!gpath || gpath[0] != '/' || strstr(gpath, "/../")) {
		// the game only uses absolute paths (its cwd is /)
		if (!gpath || !*gpath) {
			return 0;
		}
	}

	const char *rel = gpath;
	while (*rel == '/') {
		rel++;
	}

	for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++) {
		const size_t n = strlen(map[i].prefix);
		if (!strncmp(gpath, map[i].prefix, n) && (gpath[n] == '/' || gpath[n] == '\0')) {
			const char *base = map[i].which == 0 ? g_HostOpts.dataDir
				: map[i].which == 1 ? g_HostOpts.saveDir : g_HostOpts.tmpDir;
			snprintf(out, size, "%s%s", base, gpath + n);
			return 1;
		}
	}

	// anything else lives under the scratch directory
	snprintf(out, size, "%s/root/%s", g_HostOpts.tmpDir, rel);
	return 1;
}

static int hostPathFor(uint32_t gpath, char *out, size_t size)
{
	return gpath && hostPath((const char *)wptr(gpath), out, size);
}

/* ------------------------------------------------------------------------
 * file descriptors
 * ------------------------------------------------------------------------ */

#define MAX_FDS 64

static struct {
	int used;
	int fd;
	uint32_t flags;
} fds[MAX_FDS] = {
	{ 1, 0, 0 }, { 1, 1, 1 }, { 1, 2, 1 },
};

static int fdHost(uint32_t wfd)
{
	return (wfd < MAX_FDS && fds[wfd].used) ? fds[wfd].fd : -1;
}

u32 w2c_env_0x5F_syscall_openat(struct w2c_env *e, u32 dirfd, u32 path, u32 flags, u32 varargs)
{
	char hpath[1024];
	if ((int32_t)dirfd != W_AT_FDCWD || !hostPathFor(path, hpath, sizeof(hpath))) {
		return (u32)-W_EINVAL;
	}

	int hflags = 0;
	switch (flags & W_O_ACCMODE) {
	case 0: hflags = O_RDONLY; break;
	case 1: hflags = O_WRONLY; break;
	default: hflags = O_RDWR; break;
	}
	if (flags & W_O_CREAT) hflags |= O_CREAT;
	if (flags & W_O_EXCL) hflags |= O_EXCL;
	if (flags & W_O_TRUNC) hflags |= O_TRUNC;
	if (flags & W_O_APPEND) hflags |= O_APPEND;
#ifdef O_DIRECTORY
	if (flags & W_O_DIRECTORY) hflags |= O_DIRECTORY;
#endif
#ifdef O_BINARY
	hflags |= O_BINARY;
#endif

	const int mode = (flags & W_O_CREAT) && varargs ? (int)wload32(varargs) : 0644;

	int slot = -1;
	for (int i = 3; i < MAX_FDS; i++) {
		if (!fds[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return (u32)-W_EMFILE;
	}

	if (flags & W_O_CREAT) {
		// the browser's file system creates the game's directories on demand; so do we
		char dir[1024];
		snprintf(dir, sizeof(dir), "%s", hpath);
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			mkdirs(dir);
		}
	}

	const int hfd = open(hpath, hflags, mode);
	if (hfd < 0) {
		return (u32)-werrno(errno);
	}

	fds[slot].used = 1;
	fds[slot].fd = hfd;
	fds[slot].flags = flags;
	return (u32)slot;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, u32 fd)
{
	if (fd < 3) {
		return W_ESUCCESS;
	}
	const int hfd = fdHost(fd);
	if (hfd < 0) {
		return W_EBADF;
	}
	close(hfd);
	fds[fd].used = 0;
	return W_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 iovcnt, u32 nread)
{
	const int hfd = fdHost(fd);
	if (hfd < 0) {
		return W_EBADF;
	}
	uint32_t total = 0;
	for (uint32_t i = 0; i < iovcnt; i++) {
		const uint32_t buf = wload32(iovs + i * 8);
		const uint32_t len = wload32(iovs + i * 8 + 4);
		if (fd == 0) {
			break; // no console input
		}
		const ssize_t got = read(hfd, wmem() + buf, len);
		if (got < 0) {
			return (u32)werrno(errno);
		}
		total += (uint32_t)got;
		if ((uint32_t)got < len) {
			break;
		}
	}
	wstore32(nread, total);
	return W_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 iovcnt, u32 nwritten)
{
	const int hfd = fdHost(fd);
	if (hfd < 0) {
		return W_EBADF;
	}
	uint32_t total = 0;
	for (uint32_t i = 0; i < iovcnt; i++) {
		const uint32_t buf = wload32(iovs + i * 8);
		const uint32_t len = wload32(iovs + i * 8 + 4);
		uint32_t done = 0;
		while (done < len) {
			const ssize_t put = write(hfd, wmem() + buf + done, len - done);
			if (put <= 0) {
				return (u32)werrno(errno);
			}
			done += (uint32_t)put;
		}
		total += len;
	}
	wstore32(nwritten, total);
	return W_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u64 offset, u32 whence, u32 newoffset)
{
	const int hfd = fdHost(fd);
	if (hfd < 0) {
		return W_EBADF;
	}
	if (fd < 3) {
		return W_ESPIPE;
	}
	const int hw = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
	const off_t pos = lseek(hfd, (off_t)(int64_t)offset, hw);
	if (pos < 0) {
		return (u32)werrno(errno);
	}
	const uint64_t p = (uint64_t)pos;
	memcpy(wmem() + newoffset, &p, 8);
	return W_ESUCCESS;
}

u32 w2c_env_0x5F_syscall_fcntl64(struct w2c_env *e, u32 fd, u32 cmd, u32 varargs)
{
	if (fdHost(fd) < 0) {
		return (u32)-W_EBADF;
	}
	switch (cmd) {
	case W_F_GETFL:
		return fds[fd].flags;
	case W_F_GETFD:
	case W_F_SETFD:
	case W_F_SETFL:
		return 0;
	default:
		return (u32)-W_EINVAL;
	}
}

u32 w2c_env_0x5F_syscall_ioctl(struct w2c_env *e, u32 fd, u32 op, u32 varargs)
{
	// no terminals here (musl asks to decide stdout's buffering)
	return (u32)-W_ENOTTY;
}

u32 w2c_env_0x5F_syscall_faccessat(struct w2c_env *e, u32 dirfd, u32 path, u32 amode, u32 flags)
{
	char hpath[1024];
	if (!hostPathFor(path, hpath, sizeof(hpath))) {
		return (u32)-W_EINVAL;
	}
	return access(hpath, (int)amode) == 0 ? 0 : (u32)-werrno(errno);
}

u32 w2c_env_0x5F_syscall_mkdirat(struct w2c_env *e, u32 dirfd, u32 path, u32 mode)
{
	char hpath[1024];
	if (!hostPathFor(path, hpath, sizeof(hpath))) {
		return (u32)-W_EINVAL;
	}
	return mkdir(hpath, (mode_t)(mode ? mode : 0755)) == 0 ? 0 : (u32)-werrno(errno);
}

u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env *e, u32 dirfd, u32 path, u32 flags)
{
	char hpath[1024];
	if (!hostPathFor(path, hpath, sizeof(hpath))) {
		return (u32)-W_EINVAL;
	}
	const int r = (flags & W_AT_REMOVEDIR) ? rmdir(hpath) : unlink(hpath);
	return r == 0 ? 0 : (u32)-werrno(errno);
}

u32 w2c_env_0x5F_syscall_rmdir(struct w2c_env *e, u32 path)
{
	char hpath[1024];
	if (!hostPathFor(path, hpath, sizeof(hpath))) {
		return (u32)-W_EINVAL;
	}
	return rmdir(hpath) == 0 ? 0 : (u32)-werrno(errno);
}

// struct stat of Emscripten (wasm32), 96 bytes
static void writeStat(uint32_t addr, const struct stat *st)
{
	uint8_t *p = wmem() + addr;
	const uint32_t u[6] = { (uint32_t)st->st_dev, (uint32_t)st->st_mode, (uint32_t)st->st_nlink,
		(uint32_t)st->st_uid, (uint32_t)st->st_gid, (uint32_t)st->st_rdev };
	const int64_t size = (int64_t)st->st_size;
	const int32_t blksize = 4096;
	const int32_t blocks = (int32_t)((st->st_size + 511) / 512);
	const int64_t mtime = (int64_t)st->st_mtime;
	const uint64_t ino = (uint64_t)st->st_ino;

	memset(p, 0, 96);
	memcpy(p, u, sizeof(u));
	memcpy(p + 24, &size, 8);
	memcpy(p + 32, &blksize, 4);
	memcpy(p + 36, &blocks, 4);
	memcpy(p + 40, &mtime, 8); // atime
	memcpy(p + 56, &mtime, 8); // mtime
	memcpy(p + 72, &mtime, 8); // ctime
	memcpy(p + 88, &ino, 8);
}

u32 w2c_env_0x5F_syscall_stat64(struct w2c_env *e, u32 path, u32 buf)
{
	char hpath[1024];
	struct stat st;
	if (!hostPathFor(path, hpath, sizeof(hpath))) {
		return (u32)-W_EINVAL;
	}
	if (stat(hpath, &st) != 0) {
		return (u32)-werrno(errno);
	}
	writeStat(buf, &st);
	return 0;
}

/* ------------------------------------------------------------------------
 * time
 * ------------------------------------------------------------------------ */

f64 w2c_env_emscripten_date_now(struct w2c_env *e)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// struct tm of Emscripten: 9 ints, long tm_gmtoff (4), const char *tm_zone (4)
u32 w2c_env_0x5Flocaltime_js(struct w2c_env *e, u64 t, u32 tmp)
{
	const time_t tt = (time_t)(int64_t)t;
	struct tm tm;
	localtime_r(&tt, &tm);
	const int32_t v[11] = { tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year,
		tm.tm_wday, tm.tm_yday, tm.tm_isdst, 0, 0 };
	memcpy(wmem() + tmp, v, sizeof(v));
	return 0;
}

void w2c_env_0x5Ftzset_js(struct w2c_env *e, u32 timezone, u32 daylight, u32 stdName, u32 dstName)
{
	wstore32(timezone, 0);
	wstore32(daylight, 0);
	memcpy(wmem() + stdName, "UTC", 4);
	memcpy(wmem() + dstName, "UTC", 4);
}

void w2c_env_emscripten_sleep(struct w2c_env *e, u32 ms)
{
	platSleepMs(ms);
}

/* ------------------------------------------------------------------------
 * memory and exits
 * ------------------------------------------------------------------------ */

u32 w2c_env_emscripten_resize_heap(struct w2c_env *e, u32 requested)
{
	wasm_rt_memory_t *mem = w2c_pd_memory(&g_pd);
	if (requested <= mem->size) {
		return 1;
	}
	const uint64_t pages = (((uint64_t)requested - mem->size) + 65535) / 65536;
	// grow in bigger steps, like Emscripten does, so this doesn't happen often
	const uint64_t want = pages < mem->pages / 5 ? mem->pages / 5 : pages;
	if (wasm_rt_grow_memory(mem, want) != (uint64_t)-1) {
		return 1;
	}
	return wasm_rt_grow_memory(mem, pages) != (uint64_t)-1;
}

void w2c_env_exit(struct w2c_env *e, u32 code)
{
	exit((int)code);
}

void w2c_env_0x5Fabort_js(struct w2c_env *e)
{
	hostFatal("the game aborted");
}

void w2c_env_0x5F_cxa_throw(struct w2c_env *e, u32 ptr, u32 type, u32 destructor)
{
	hostFatal("unhandled C++ exception in the game");
}
