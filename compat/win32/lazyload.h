#ifndef LAZYLOAD_H
#define LAZYLOAD_H

/* simplify loading of DLL functions */

struct proc_addr {
	const char *const dll;
	const char *const function;
	FARPROC pfunction;
	unsigned initialized : 1;
};

/* Declares a function to be loaded dynamically from a DLL. */
#define DECLARE_PROC_ADDR(dll, rettype, function, ...) \
	static struct proc_addr proc_addr_##function = \
	{ #dll, #function, NULL, 0 }; \
	static rettype (WINAPI *function)(__VA_ARGS__)

/*
 * Loads a function from a DLL (once-only).
 * Returns non-NULL function pointer on success.
 * Returns NULL + errno == ENOSYS on failure.
 */
#define INIT_PROC_ADDR(function) (function = get_proc_addr(&proc_addr_##function))

#ifdef _MSC_VER
#define inline __inline
#endif

static inline void *get_proc_addr(struct proc_addr *proc)
{
	/* only do this once */
	if (!proc->initialized) {
		HANDLE hnd;
		proc->initialized = 1;
		hnd = LoadLibraryA(proc->dll);
		if (hnd)
			proc->pfunction = GetProcAddress(hnd, proc->function);
	}
	/* set ENOSYS if DLL or function was not found */
	if (!proc->pfunction)
		errno = ENOSYS;
	return proc->pfunction;
}

#endif
