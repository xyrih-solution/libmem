#include "internal.h"

#if LM_OS == LM_OS_WIN
typedef struct {
	lm_bool_t(*callback)(lm_page_t page, lm_void_t *arg);
	lm_void_t *arg;
} _lm_enum_pages_t;

LM_PRIVATE lm_bool_t
_LM_EnumPagesCallback(lm_module_t  mod,
		      lm_tstring_t modpath,
		      lm_void_t   *arg)
{
	lm_address_t addr;
	_lm_enum_pages_t *parg = (_lm_enum_pages_t *)arg;
	MEMORY_BASIC_INFORMATION mbi;
	lm_page_t page;

	for (addr = mod.base;
	     VirtualQuery(addr, &mbi, sizeof(mbi));
	     addr = (lm_address_t)LM_OFFSET(mbi.BaseAddress, mbi.RegionSize)) {
		page.base  = (lm_address_t)mbi.BaseAddress;
		page.size  = (lm_size_t)mbi.RegionSize;
		page.end   = (lm_address_t)(
			&((lm_byte_t *)page.base)[page.size]
		);
		page.prot  = mbi.Protect;
		page.flags = mbi.Type;

		if (parg->callback(page, parg->arg) == LM_FALSE)
			return LM_FALSE;
	}

	return LM_TRUE;
}

LM_PRIVATE lm_bool_t
_LM_EnumPages(lm_bool_t(*callback)(lm_page_t  page,
				   lm_void_t *arg),
	      lm_void_t *arg)
{
	_lm_enum_pages_t data;

	data.callback = callback;
	data.arg = arg;

	return _LM_EnumModules(_LM_EnumPagesCallback, (lm_void_t *)&data);
}
#else
LM_PRIVATE lm_bool_t
_LM_EnumPages(lm_bool_t(*callback)(lm_page_t  page,
				   lm_void_t *arg),
	      lm_void_t *arg)
{
	return LM_EnumPagesEx(LM_GetProcessId(), callback, arg);
}
#endif

LM_API lm_bool_t
LM_EnumPages(lm_bool_t(*callback)(lm_page_t  page,
				  lm_void_t *arg),
	     lm_void_t *arg)
{
	LM_ASSERT(callback != LM_NULLPTR);

	return _LM_EnumPages(callback, arg);
}

/********************************/

#if LM_OS == LM_OS_WIN
typedef struct {
	HANDLE hProcess;
	lm_bool_t(*callback)(lm_page_t page, lm_void_t *arg);
	lm_void_t *arg;
} _lm_enum_pages_ex_t;

LM_PRIVATE lm_bool_t
_LM_EnumPagesExCallback(lm_module_t  mod,
			lm_tstring_t modpath,
			lm_void_t   *arg)
{
	lm_address_t addr;
	_lm_enum_pages_ex_t *parg = (_lm_enum_pages_t *)arg;
	MEMORY_BASIC_INFORMATION mbi;
	lm_page_t page;

	for (addr = mod.base;
	     VirtualQueryEx(parg->hProcess, addr, &mbi, sizeof(mbi));
	     addr = (lm_address_t)LM_OFFSET(mbi.BaseAddress, mbi.RegionSize)) {
		page.base  = (lm_address_t)mbi.BaseAddress;
		page.size  = (lm_size_t)mbi.RegionSize;
		page.end   = (lm_address_t)(
			&((lm_byte_t *)page.base)[page.size]
		);
		page.prot  = mbi.Protect;
		page.flags = mbi.Type;

		if (parg->callback(page, parg->arg) == LM_FALSE)
			return LM_FALSE;
	}

	return LM_TRUE;
}

LM_PRIVATE lm_bool_t
_LM_EnumPagesEx(lm_pid_t   pid,
		lm_bool_t(*callback)(lm_page_t  page,
				     lm_void_t *arg),
		lm_void_t *arg)
{
	lm_bool_t ret = LM_FALSE;
	_lm_enum_pages_ex_t data;

	if (!_LM_OpenProcess(pid, &data.hProcess))
		return ret;

	data.callback = callback;
	data.arg = arg;

	ret = _LM_EnumModulesEx(pid, _LM_EnumPagesExCallback, (lm_void_t *)&data);

	_LM_CloseProcess(&data.hProcess);

	return ret;
}
#else
LM_PRIVATE lm_bool_t
_LM_EnumPagesEx(lm_pid_t   pid,
		lm_bool_t(*callback)(lm_page_t  page,
				     lm_void_t *arg),
		lm_void_t *arg)
{
	lm_bool_t   ret = LM_FALSE;
	lm_tchar_t *maps_line = NULL;
	size_t      maps_line_len;
	lm_tchar_t  maps_path[LM_PATH_MAX] = { 0 };
	FILE       *maps_file;
	regex_t     regex;
	regmatch_t  matches[4];
	lm_page_t   page;
	size_t      i;

#	if LM_OS == LM_OS_BSD
	if (regcomp(&regex, "^0x([a-z0-9]+)[[:blank:]]+0x([a-z0-9]+).*0x[a-z0-9]+[[:blank:]]+(.+).*$", REG_EXTENDED))
		return ret;

	LM_SNPRINTF(maps_path, LM_ARRLEN(maps_path),
		    LM_STR("%s/%d/map"), LM_PROCFS, pid);
#	else
	if (regcomp(&regex, "^([a-z0-9]+)-([a-z0-9]+)[[:blank:]]+(.+).*$", REG_EXTENDED))
		return ret;

	LM_SNPRINTF(maps_path, LM_ARRLEN(maps_path),
		    LM_STR("%s/%d/maps"), LM_PROCFS, pid);
#	endif

	maps_file = LM_FOPEN(maps_path, "r");
	if (!maps_file)
		goto FREE_EXIT;

	while (LM_GETLINE(&maps_line, &maps_line_len, maps_file)) {
		if (regexec(&regex, maps_line, LM_ARRLEN(matches), matches, 0))
			continue;

		page.base = (lm_address_t)LM_STRTOP(
			&maps_line[matches[1].rm_so], NULL, 16
		);
		page.end = (lm_address_t)LM_STRTOP(
			&maps_line[matches[2].rm_so], NULL, 16
		);

		page.prot = 0;
		page.flags = 0;
		for (i = 0; i < (size_t)(matches[3].rm_eo - matches[3].rm_so); ++i) {
			switch (maps_line[matches[3].rm_so + i]) {
			case 'r': page.prot |= PROT_READ; break;
			case 'w': page.prot |= PROT_WRITE; break;
			case 'x': page.prot |= PROT_EXEC; break;
#			if LM_OS != LM_OS_BSD
			case 'p': page.flags |= MAP_PRIVATE; break;
			case 's': page.flags |= MAP_SHARED; break;
#			endif
			}
		}
		page.size = (lm_size_t)(
			(lm_uintptr_t)page.end - (lm_uintptr_t)page.base
		);

		if (callback(page, arg) == LM_FALSE)
			break;
	}

	ret = LM_TRUE;

	LM_FCLOSE(maps_file);
FREE_EXIT:
	regfree(&regex);

	return ret;
}
#endif

LM_API lm_bool_t
LM_EnumPagesEx(lm_pid_t   pid,
	       lm_bool_t(*callback)(lm_page_t  page,
				    lm_void_t *arg),
	       lm_void_t *arg)
{
	LM_ASSERT(pid != LM_PID_BAD && callback != LM_NULLPTR);

	return _LM_EnumPagesEx(pid, callback, arg);
}

/********************************/

#if LM_OS == LM_OS_WIN
LM_PRIVATE lm_bool_t
_LM_GetPage(lm_address_t addr,
	    lm_page_t   *page)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
		return LM_FALSE;

	page->base  = (lm_address_t)mbi.BaseAddress;
	page->size  = (lm_size_t)mbi.RegionSize;
	page->end   = (lm_address_t)LM_OFFSET(page.base, page.size);
	page->prot  = mbi.Protect;
	page->flags = mbi.Type;

	return LM_TRUE;
}
#else
typedef struct {
	lm_address_t addr;
	lm_page_t   *pagebuf;
} _lm_get_page_t;

LM_PRIVATE lm_bool_t
_LM_GetPageCallback(lm_page_t  page,
		    lm_void_t *arg)
{
	_lm_get_page_t *parg = (_lm_get_page_t *)arg;
	
	if ((lm_uintptr_t)parg->addr >= (lm_uintptr_t)page.base &&
	    (lm_uintptr_t)parg->addr < (lm_uintptr_t)page.end) {
		*parg->pagebuf = page;
		return LM_FALSE;
	}

	return LM_TRUE;
}

LM_PRIVATE lm_bool_t
_LM_GetPage(lm_address_t addr,
	    lm_page_t   *page)
{
	lm_bool_t ret = LM_FALSE;
	_lm_get_page_t arg;

	LM_ASSERT(addr != LM_ADDRESS_BAD && page != LM_NULLPTR);

	arg.addr = addr;
	arg.pagebuf = page;
	arg.pagebuf->base = LM_ADDRESS_BAD;
	arg.pagebuf->size = 0;
	arg.pagebuf->end  = LM_ADDRESS_BAD;

	LM_EnumPages(_LM_GetPageCallback, (lm_void_t *)&arg);

	ret = page->size > 0 ? LM_TRUE : LM_FALSE;

	return ret;
}
#endif

LM_API lm_bool_t
LM_GetPage(lm_address_t addr,
	   lm_page_t   *page)
{
	return _LM_GetPage(addr, page);
}

/********************************/

#if LM_OS == LM_OS_WIN
LM_PRIVATE lm_bool_t
_LM_GetPageEx(lm_pid_t     pid,
	      lm_address_t addr,
	      lm_page_t   *page)
{
	lm_bool_t ret = LM_FALSE;
	HANDLE hProcess;
	MEMORY_BASIC_INFORMATION mbi;

	if (!_LM_OpenProcess(pid, &hProcess))
		return ret;

	if (!VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)))
		goto CLOSE_RET;

	page->base  = (lm_address_t)mbi.BaseAddress;
	page->size  = (lm_size_t)mbi.RegionSize;
	page->end   = (lm_address_t)LM_OFFSET(page.base, page.size);
	page->prot  = mbi.Protect;
	page->flags = mbi.Type;

	ret = LM_TRUE;
CLOSE_RET:
	_LM_CloseProcess(&hProcess);
	return ret;
}
#else
LM_PRIVATE lm_bool_t
_LM_GetPageEx(lm_pid_t     pid,
	      lm_address_t addr,
	      lm_page_t   *page)
{
	lm_bool_t ret = LM_FALSE;
	_lm_get_page_t arg;

	LM_ASSERT(pid != LM_PID_BAD &&
		  addr != LM_ADDRESS_BAD &&
		  page != LM_NULLPTR);

	arg.addr = addr;
	arg.pagebuf = page;
	arg.pagebuf->base = LM_ADDRESS_BAD;
	arg.pagebuf->size = 0;
	arg.pagebuf->end  = LM_ADDRESS_BAD;

	LM_EnumPagesEx(pid, _LM_GetPageCallback, (lm_void_t *)&arg);

	ret = page->size > 0 ? LM_TRUE : LM_FALSE;
	return ret;
}
#endif

LM_API lm_bool_t
LM_GetPageEx(lm_pid_t     pid,
	     lm_address_t addr,
	     lm_page_t   *page)
{
	return _LM_GetPageEx(pid, addr, page);
}

