// misc. POSIX routines for Win32
//
// Copyright (c) 2004 Jan Wassenberg
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// Contact info:
//   Jan.Wassenberg@stud.uni-karlsruhe.de
//   http://www.stud.uni-karlsruhe.de/~urkt/

// collection of hacks :P

#include "precompiled.h"

#include "lib.h"
#include "win_internal.h"

#include <process.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>


// cast intptr_t to HANDLE; centralized for easier changing, e.g. avoiding
// warnings. i = -1 converts to INVALID_HANDLE_VALUE (same value).
static inline HANDLE cast_to_HANDLE(intptr_t i)
{
	return (HANDLE)((char*)0 + i);
}


//////////////////////////////////////////////////////////////////////////////
//
// file
//
//////////////////////////////////////////////////////////////////////////////


int open(const char* fn, int oflag, ...)
{
	const bool is_com_port = strncmp(fn, "/dev/tty", 8) == 0;
		// also used later, before aio_reopen

	// "/dev/tty?" => "COM?"
	if(is_com_port)
	{
		char port[] = "COM ";
		// Windows only supports COM1..COM4.
		char c = fn[8]+1;
		if(!('1' <= c && c <= '4'))
			return -1;
		port[3] = (char)(fn[8]+1);
		fn = port;
	}

	mode_t mode = 0;
	if(oflag & O_CREAT)
	{
		va_list args;
		va_start(args, oflag);
		mode = va_arg(args, mode_t);
		va_end(args);
	}

	WIN_SAVE_LAST_ERROR;	// CreateFile
	int fd = _open(fn, oflag, mode);
	WIN_RESTORE_LAST_ERROR;

#ifdef PARANOIA
debug_out("open %s = %d\n", fn, fd);
#endif

	// open it for async I/O as well (_open defaults to deny_none sharing):
	// if not stdin/stdout/stderr and
	if(fd > 2)
	{
		// not a COM port. don't currently need aio access for those;
		// also, aio_reopen's CreateFile reports access denied when trying to open.
		if(!is_com_port)
			aio_reopen(fd, fn, oflag);
	}

	// CRT doesn't like more than 255 files open.
	// warn now, so that we notice why so many are open
#ifndef NDEBUG
	if(fd > 256)
		debug_warn("wposix: too many files open (CRT limitation)");
#endif

	return fd;
}


int close(int fd)
{
#ifdef PARANOIA
debug_out("close %d\n", fd);
#endif

	assert(3 <= fd && fd < 256);
	aio_close(fd);
	return _close(fd);
}


int ioctl(int fd, int op, int* data)
{
	const HANDLE h = cast_to_HANDLE(_get_osfhandle(fd));

	switch(op)
	{
	case TIOCMGET:
		/* TIOCM_* mapped directly to MS_*_ON */
		GetCommModemStatus(h, (DWORD*)data);
		break;

	case TIOCMBIS:
		/* only RTS supported */
		if(*data & TIOCM_RTS)
			EscapeCommFunction(h, SETRTS);
		else
			EscapeCommFunction(h, CLRRTS);
		break;

	case TIOCMIWAIT:
		static DWORD mask;
		DWORD new_mask = 0;
		if(*data & TIOCM_CD)
			new_mask |= EV_RLSD;
		if(*data & TIOCM_CTS)
			new_mask |= EV_CTS;
		if(new_mask != mask)
			SetCommMask(h, mask = new_mask);
		WaitCommEvent(h, &mask, 0);
		break;
	}

	return 0;
}




// from wtime
extern time_t local_filetime_to_time_t(FILETIME* ft);
extern time_t utc_filetime_to_time_t(FILETIME* ft);

// convert Windows FILETIME to POSIX time_t (seconds-since-1970 UTC);
// used by stat and readdir_stat_np for st_mtime.
//
// path is used to determine the file system that recorded the time
// (workaround for a documented Windows bug in converting FAT file times)
//
// note: complicated because we need to ensure the time returned is correct:
// VFS mount logic considers files 'equal' if mtime and size are the same.
static time_t filetime_to_time_t(FILETIME* ft, const char* path)
{
	// determine file system on volume containing path:
	// .. assume relative path
	const char* root = 0;
	char drive_str[] = "?:\\";
	// .. it's an absolute path
	if(isalpha(path[0]) && path[1] == ':' && path[2] == '\\')
	{
		drive_str[0] = path[0];	// drive letter
		root = drive_str;
	}
	char fs_name[16] = { 0 };
	GetVolumeInformation(root, 0,0,0,0,0, fs_name, sizeof(fs_name));
		// if this fails, fs_name != "FAT" => special-case is skipped.

	// the FAT file system stores local file times, while
	// NTFS records UTC. Windows does convert automatically,
	// but uses the current DST settings. (boo!)
	// we go back to local time, and convert properly.
	if(!strncmp(fs_name, "FAT", 3))	// e.g. FAT32
	{
		FILETIME local_ft;
		FileTimeToLocalFileTime(ft, &local_ft);
		return local_filetime_to_time_t(&local_ft);
	}

	return utc_filetime_to_time_t(ft);
}

/*
// currently only sets st_mode (file or dir) and st_size.
int stat(const char* fn, struct stat* s)
{
	memset(s, 0, sizeof(struct stat));

	WIN32_FILE_ATTRIBUTE_DATA fad;
	if(!GetFileAttributesEx(fn, GetFileExInfoStandard, &fad))
		return -1;

	s->st_mtime = fad.ftLastAccessTime

	// dir
	if(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		s->st_mode = S_IFDIR;
	else
	{
		s->st_mode = S_IFREG;
		s->st_size = (off_t)((((u64)fad.nFileSizeHigh) << 32) | fad.nFileSizeLow);
	}

	return 0;
}
*/


//////////////////////////////////////////////////////////////////////////////
//
// dir
//
//////////////////////////////////////////////////////////////////////////////


char* realpath(const char* fn, char* path)
{
	if(!GetFullPathName(fn, PATH_MAX, path, 0))
		return 0;
	return path;
}


int mkdir(const char* path, mode_t)
{
	return CreateDirectory(path, 0)? 0 : -1;
}


// opendir/readdir/closedir
//
// implementation rationale:
//
// opendir only performs minimal error checks (does directory exist?);
// readdir calls FindFirstFile. this is to ensure correct handling
// of empty directories. we need to store the path in WDIR anyway
// for filetime_to_time_t.
// 
// we avoid opening directories or returning files that have hidden or system
// attributes set. this is to prevent returning something like
// "\system volume information", which raises an error upon opening.

struct WDIR
{
	HANDLE hFind;
	WIN32_FIND_DATA fd;

	struct dirent ent;
		// can't be global - must not be overwritten
		// by calls from different DIRs.

	char path[PATH_MAX+1];
		// can't be stored in fd or ent's path fields -
		// needed by each readdir_stat_np (for filetime_to_time_t).
};


static const DWORD hs = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
	// convenience

DIR* opendir(const char* path)
{
	// make sure path exists and is a normal directory (see rationale above).
	// note: this is the only error check we can do here -
	// FindFirstFile is called in readdir (see rationale above).
	DWORD fa = GetFileAttributes(path);
	if((fa == INVALID_FILE_ATTRIBUTES) || !(fa & FILE_ATTRIBUTE_DIRECTORY) || (fa & hs))
		return 0;

	WDIR* d = (WDIR*)calloc(sizeof(WDIR), 1);
		// zero-initializes everything (required).

	// note: "path\\dir" only returns information about that directory;
	// trailing slashes aren't allowed. we have to append "\\*" to find files.
	strncpy(d->path, path, MAX_PATH-2);
	strcat(d->path, "\\*");

	return d;
}


struct dirent* readdir(DIR* d_)
{
	WDIR* const d = (WDIR*)d_;

	DWORD prev_err = GetLastError();

	// bails if end of dir reached or error.
	// called (again) if entry was rejected below.
get_another_entry:

	// first time
	if(d->hFind == 0)
	{
		d->hFind = FindFirstFile(d->path, &d->fd);
		if(d->hFind != INVALID_HANDLE_VALUE)    // success
			goto have_entry;
	}
	else
		if(FindNextFile(d->hFind, &d->fd))      // success
			goto have_entry;

	// Find*File failed; determine why and bail.
	// .. legit, end of dir reached. don't pollute last error code.
	if(GetLastError() == ERROR_NO_MORE_FILES)
		SetLastError(prev_err);
	else
		debug_warn("readdir: Find*File failed");
	return 0;


	// d->fd holds a valid entry, but we may have to get another below.
have_entry:

	// we must not return hidden or system entries, so get another.
	// (see rationale above).
	if(d->fd.dwFileAttributes & hs)
		goto get_another_entry;


	// this entry has passed all checks; return information about it.
	// .. d_ino zero-initialized by opendir
	// .. POSIX requires d_name to be an array, so we copy there.
	strncpy(d->ent.d_name, d->fd.cFileName, PATH_MAX);
	return &d->ent;
}


// return status for the dirent returned by the last successful
// readdir call from the given directory stream.
// currently sets st_size, st_mode, and st_mtime; the rest are zeroed.
// non-portable, but considerably faster than stat(). used by file_enum.
int readdir_stat_np(DIR* d_, struct stat* s)
{
	WDIR* d = (WDIR*)d_;

	memset(s, 0, sizeof(struct stat));
	s->st_size  = (off_t)((((u64)d->fd.nFileSizeHigh) << 32) | d->fd.nFileSizeLow);
	s->st_mode  = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)? S_IFDIR : S_IFREG;
	s->st_mtime = filetime_to_time_t(&d->fd.ftLastWriteTime, d->path);
	return 0;
}


int closedir(DIR* d_)
{
	WDIR* const d = (WDIR*)d_;

	FindClose(d->hFind);

	memset(d, 0, sizeof(WDIR));	// safety
	free(d);
	return 0;
}


//////////////////////////////////////////////////////////////////////////////
//
// terminal
//
//////////////////////////////////////////////////////////////////////////////


static HANDLE std_h[2] = { (HANDLE)((char*)0 + 3), (HANDLE)((char*)0 + 7) };


__declspec(naked) void _get_console()
{ __asm	jmp		dword ptr [AllocConsole] }

__declspec(naked) void _hide_console()
{ __asm jmp		dword ptr [FreeConsole] }


int tcgetattr(int fd, struct termios* termios_p)
{
	if(fd > 2)
		return -1;
	HANDLE h = std_h[fd];

	DWORD mode;
	GetConsoleMode(h, &mode);
	termios_p->c_lflag = mode & (ENABLE_ECHO_INPUT|ENABLE_LINE_INPUT);

	return 0;
}


int tcsetattr(int fd, int /* optional_actions */, const struct termios* termios_p)
{
	if(fd > 2)
		return -1;
	HANDLE h = std_h[fd];
	SetConsoleMode(h, (DWORD)termios_p->c_lflag);
	FlushConsoleInputBuffer(h);

	return 0;
}


int poll(struct pollfd /* fds */[], int /* nfds */, int /* timeout */)
{
	return -1;
}


//////////////////////////////////////////////////////////////////////////////
//
// thread
//
//////////////////////////////////////////////////////////////////////////////


__declspec(naked) pthread_t pthread_self(void)
{ __asm jmp		dword ptr [GetCurrentThread] }


int pthread_getschedparam(pthread_t thread, int* policy, struct sched_param* param)
{
	if(policy)
	{
		DWORD pc = GetPriorityClass(GetCurrentProcess());
		*policy = (pc >= HIGH_PRIORITY_CLASS)? SCHED_FIFO : SCHED_RR;
	}
	if(param)
	{
		const HANDLE hThread = cast_to_HANDLE((intptr_t)thread);
		param->sched_priority = GetThreadPriority(hThread);
	}

	return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param* param)
{
	const int pri = param->sched_priority;

	// additional boost for policy == SCHED_FIFO
	DWORD pri_class = NORMAL_PRIORITY_CLASS;
	if(policy == SCHED_FIFO)
	{
		pri_class = HIGH_PRIORITY_CLASS;
		if(pri == 2)
			pri_class = REALTIME_PRIORITY_CLASS;
	}
	SetPriorityClass(GetCurrentProcess(), pri_class);

	// choose fixed Windows values from pri
	const HANDLE hThread = cast_to_HANDLE((intptr_t)thread);
	SetThreadPriority(hThread, pri);
	return 0;
}


struct ThreadParam
{
	void*(*func)(void*);
	void* user_arg;
	ThreadParam(void*(*_func)(void*), void* _user_arg)
		: func(_func), user_arg(_user_arg) {}
};


// trampoline to switch calling convention.
// param points to a heap-allocated ThreadParam (see pthread_create).
static unsigned __stdcall thread_start(void* param)
{
	ThreadParam* f = (ThreadParam*)param;
	void*(*func)(void*) = f->func;
	void* user_arg      = f->user_arg;
	delete f;

	// workaround for stupid "void* -> unsigned cast" warning
	union { void* p; unsigned u; } v;
	v.p = func(user_arg);
	return v.u;
}


int pthread_create(pthread_t* thread, const void* attr, void*(*func)(void*), void* user_arg)
{
	UNUSED(attr);

	// notes:
	// - don't call via asm: _beginthreadex might be a func ptr (if DLL CRT).
	// - don't stack-allocate param: thread_start might not be called
	//   in the new thread before we exit this stack frame.
	ThreadParam* param = new ThreadParam(func, user_arg);
	*thread = (pthread_t)_beginthreadex(0, 0, thread_start, (void*)param, 0, 0);
	return 0;
}


void pthread_cancel(pthread_t thread)
{
	HANDLE hThread = cast_to_HANDLE((intptr_t)thread);
	TerminateThread(hThread, 0);
}


void pthread_join(pthread_t thread, void** value_ptr)
{
	HANDLE hThread = cast_to_HANDLE((intptr_t)thread);

	// clean exit
	if(WaitForSingleObject(hThread, 100) == WAIT_OBJECT_0)
	{
		if(value_ptr)
			GetExitCodeThread(hThread, (LPDWORD)value_ptr);
	}
	// force close
	else
		TerminateThread(hThread, 0);
	if(value_ptr)
		*value_ptr = (void*)-1;
	CloseHandle(hThread);	
}


// DeleteCriticalSection currently doesn't complain if we double-free
// (e.g. user calls destroy() and static initializer atexit runs),
// and dox are ambiguous.
/*
struct CS
{
	CRITICAL_SECTION cs;
	CS()
	{
		InitializeCriticalSection(&cs);
	}
	~CS()
	{
		DeleteCriticalSection(&cs);
	}
};*/

cassert(sizeof(CRITICAL_SECTION) == sizeof(pthread_mutex_t));
/*
static std::list<CS> mutexes;

static void destroy_mutexes()
{
	mutexes.clear();
}
*/

pthread_mutex_t pthread_mutex_initializer()
{
	CRITICAL_SECTION cs;
	InitializeCriticalSection(&cs);
	return *(pthread_mutex_t*)&cs;
}

int pthread_mutex_destroy(pthread_mutex_t* m)
{
	DeleteCriticalSection((CRITICAL_SECTION*)m);
	return 0;
/*
	CS* cs = (CS*)m;
	mutexes.erase(cs);
	delete cs;
	return 0;
*/
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t*)
{
	InitializeCriticalSection((CRITICAL_SECTION*)m);
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m)
{
	EnterCriticalSection((CRITICAL_SECTION*)m);
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* m)
{
	BOOL got_it = TryEnterCriticalSection((CRITICAL_SECTION*)m);
	return got_it? 0 : -1;
}

int pthread_mutex_unlock(pthread_mutex_t* m)
{
	LeaveCriticalSection((CRITICAL_SECTION*)m);
	return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs_timeout)
{
	return -ENOSYS;
}




int sem_init(sem_t* sem, int pshared, unsigned value)
{
	*sem = (uintptr_t)CreateSemaphore(0, (LONG)value, 0x7fffffff, 0);
	return 0;
}

int sem_post(sem_t* sem)
{
	ReleaseSemaphore((HANDLE)*sem, 1, 0);
	return 0;
}

int sem_wait(sem_t* sem)
{
	WaitForSingleObject((HANDLE)*sem, INFINITE);
	return 0;
}

int sem_destroy(sem_t* sem)
{
	CloseHandle((HANDLE)*sem);
	return 0;
}


//////////////////////////////////////////////////////////////////////////////
//
// memory mapping
//
//////////////////////////////////////////////////////////////////////////////


void* mmap(void* user_start, size_t len, int prot, int flags, int fd, off_t offset)
{
	{
	WIN_SAVE_LAST_ERROR;

	// assume fd = -1 (requesting mapping backed by page file),
	// so that we notice invalid file handles below.
	HANDLE hFile = INVALID_HANDLE_VALUE;
	if(fd != -1)
	{
		hFile = cast_to_HANDLE(_get_osfhandle(fd));
		if(hFile == INVALID_HANDLE_VALUE)
		{
			debug_warn("mmap: invalid file handle");
			goto fail;
		}
	}

	// MapView.. will choose start address unless MAP_FIXED was specified.
	void* start = 0;
	if(flags & MAP_FIXED)
	{
		start = user_start;
		if(start == 0)	// assert below would fire
			goto fail;
	}

	// figure out access rights.
	// note: reads are always allowed (Win32 limitation).

	SECURITY_ATTRIBUTES sec = { sizeof(SECURITY_ATTRIBUTES), (void*)0, FALSE };
	DWORD flProtect = PAGE_READONLY;
	DWORD dwAccess = FILE_MAP_READ;

	// .. no access: not possible on Win32.
	if(prot == PROT_NONE)
		goto fail;
	// .. write or read/write (Win32 doesn't support write-only)
	if(prot & PROT_WRITE)
	{
		flProtect = PAGE_READWRITE;

		const bool shared = (flags & MAP_SHARED ) != 0;
		const bool priv   = (flags & MAP_PRIVATE) != 0;
		// .. both aren't allowed
		if(shared && priv)
			goto fail;
		// .. changes are shared & written to file
		else if(shared)
		{
			sec.bInheritHandle = TRUE;
			dwAccess = FILE_MAP_ALL_ACCESS;
		}
		// .. private copy-on-write mapping
		else if(priv)
		{
			flProtect = PAGE_WRITECOPY;
			dwAccess = FILE_MAP_COPY;
		}
	}

	// now actually map.
	const DWORD len_hi = (DWORD)((u64)len >> 32);
		// careful! language doesn't allow shifting 32-bit types by 32 bits.
	const DWORD len_lo = (DWORD)len & 0xffffffff;
	const HANDLE hMap = CreateFileMapping(hFile, &sec, flProtect, len_hi, len_lo, (LPCSTR)0);
	if(hMap == INVALID_HANDLE_VALUE)
		// bail now so that MapView.. doesn't overwrite the last error value.
		goto fail;
	void* ptr = MapViewOfFileEx(hMap, dwAccess, len_hi, offset, len_lo, start);

	// free the mapping object now, so that we don't have to hold on to its
	// handle until munmap(). it's not actually released yet due to the
	// reference held by MapViewOfFileEx (if it succeeded).
	if(hMap != INVALID_HANDLE_VALUE)	// avoid "invalid handle" error
		CloseHandle(hMap);

	if(!ptr)
		// bail now, before the last error value is restored,
		// but after freeing the mapping object.
		goto fail;

	assert(!(flags & MAP_FIXED) || (ptr == start));
		// fixed => ptr = start

	WIN_RESTORE_LAST_ERROR;

	return ptr;
	}
fail:
	return MAP_FAILED;
}


int munmap(void* start, size_t len)
{
	UNUSED(len);
	BOOL ok = UnmapViewOfFile(start);
	return ok? 0 : -1;
}




int uname(struct utsname* un)
{
	static OSVERSIONINFO vi;
	vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&vi);

	// OS implementation name
	const char* family = "??";
	int ver = (vi.dwMajorVersion << 8) | vi.dwMinorVersion;
	if(vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
		family = (ver == 0x045a)? "ME" : "9x";
	if(vi.dwPlatformId == VER_PLATFORM_WIN32_NT)
	{
		if(ver == 0x0500)
			family = "2k";
		else if(ver == 0x0501)
			family = "XP";
		else
			family = "NT";
	}
	sprintf(un->sysname, "Win%s", family);

	// release info
	const char* vs = vi.szCSDVersion;
	int sp;
	if(sscanf(vs, "Service Pack %d", &sp) == 1)
		sprintf(un->release, "SP %d", sp);
	else
	{
		const char* release = "";
		if(vi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
		{
			if(!strcmp(vs, " C"))
				release = "OSR2";
			else if(!strcmp(vs, " A"))
				release = "SE";
		}
		strcpy(un->release, release);
	}

	// version
	sprintf(un->version, "%lu.%02lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber & 0xffff);

	// node name
	DWORD buf_size = sizeof(un->nodename);
	DWORD last_err = GetLastError();
	BOOL ok = GetComputerName(un->nodename, &buf_size);
	// GetComputerName sets last error even on success - suppress.
	if(ok)
		SetLastError(last_err);
	else
		debug_warn("GetComputerName failed");

	// hardware type
	static SYSTEM_INFO si;
	GetSystemInfo(&si);
	if(si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
		strcpy(un->machine, "AMD64");
	else
		strcpy(un->machine, "IA-32");

	return 0;
}


 




long sysconf(int name)
{
	// used by _SC_*_PAGES
	static DWORD page_size;
	static BOOL (WINAPI *pGlobalMemoryStatusEx)(MEMORYSTATUSEX*);  

	ONCE(
	{
		// get page size
		// (used by _SC_PAGESIZE and _SC_*_PAGES)
		SYSTEM_INFO si;
		GetSystemInfo(&si);		// can't fail => page_size always > 0.
		page_size = si.dwPageSize;

		// import GlobalMemoryStatusEx - it's not defined by the VC6 PSDK.
		// used by _SC_*_PAGES if available (provides better results).
		const HMODULE hKernel32Dll = LoadLibrary("kernel32.dll");  
		*(void**)&pGlobalMemoryStatusEx = GetProcAddress(hKernel32Dll, "GlobalMemoryStatusEx"); 
		FreeLibrary(hKernel32Dll);
			// make sure the reference is released so BoundsChecker
			// doesn't complain. it won't actually be unloaded anyway -
			// there is at least one other reference.
	}
	);


	switch(name)
	{
	case _SC_PAGESIZE:
		return page_size;

	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES:
		{
		u64 total_phys_mem;
		u64 avail_phys_mem;

		// first try GlobalMemoryStatus - cannot fail.
		// override its results if GlobalMemoryStatusEx is available.
		MEMORYSTATUS ms;
		GlobalMemoryStatus(&ms);
			// can't fail.
		total_phys_mem = ms.dwTotalPhys;
		avail_phys_mem = ms.dwAvailPhys;

		// newer API is available: use it to report correct results
		// (no overflow or wraparound) on systems with > 4 GB of memory.
		MEMORYSTATUSEX mse = { sizeof(mse) };
		if(pGlobalMemoryStatusEx && pGlobalMemoryStatusEx(&mse))
		{
			total_phys_mem = mse.ullTotalPhys;
			avail_phys_mem = mse.ullAvailPhys;
		}
		// else: not an error, since this isn't available before Win2k / XP.
		// we have results from GlobalMemoryStatus anyway.

		if(name == _SC_PHYS_PAGES)
			return (long)(round_up((uintptr_t)total_phys_mem, 2*MB) / page_size);
				// Richter, "Programming Applications for Windows":
				// reported value doesn't include non-paged pool reserved
				// during boot; it's not considered available to kernel.
				// it's 528 KB on my 512 MB machine (WinXP and Win2k).
 		else
			return (long)(avail_phys_mem / page_size);
		}

	default:
		return -1;
	}
}
