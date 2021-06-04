#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <lua.h>
#include <stdio.h>

#include "malloc_hook.h"
#include "skynet.h"
#include "atomic.h"

// turn on MEMORY_CHECK can do more memory check, such as double free
// #define MEMORY_CHECK

#define MEMORY_ALLOCTAG 0x20140605
#define MEMORY_FREETAG 0x0badf00d

static ATOM_SIZET _used_memory = 0;//记录申请了多少内存
static ATOM_SIZET _memory_block = 0;//记录申请了多少次内存

struct mem_data {
	uint32_t handle;//记录线程id
	ssize_t allocated;//记录当前线程申请了多少内存
};

struct mem_cookie {
	uint32_t handle;//记录线程id
#ifdef MEMORY_CHECK
	uint32_t dogtag;
#endif
};

#define SLOT_SIZE 0x10000//最大线程数
#define PREFIX_SIZE sizeof(struct mem_cookie)

static struct mem_data mem_stats[SLOT_SIZE];


#ifndef NOUSE_JEMALLOC

#include "jemalloc.h"//JeMalloc分配器

// for skynet_lalloc use
#define raw_realloc je_realloc
#define raw_free je_free


/*
为了防止内存泄漏，所以记录释放和请求。
陈硕给了一种办法：使用share_ptr和week_ptr可以解决循环引用的问题，但从根本上来说，skynet更绝对。前者偏向开发者，后者是作为框架设计者。
当然，这里的记录是没意义的记录。
*/
static ssize_t*
get_allocated_field(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));
	struct mem_data *data = &mem_stats[h];
	uint32_t old_handle = data->handle;
	ssize_t old_alloc = data->allocated;
	if(old_handle == 0 || old_alloc <= 0) {
		// data->allocated may less than zero, because it may not count at start.
		//初始化一下
		if(!ATOM_CAS(&data->handle, old_handle, handle)) {
			return 0;
		}
		if (old_alloc < 0) {
			ATOM_CAS(&data->allocated, old_alloc, 0);//怎么可以减得小于零呢？
		}
	}
	if(data->handle != handle) {//用handle查的handle却不等于自己
		return 0;//这里已经出错了。
	}
	return &data->allocated;
}

inline static void
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	ATOM_FADD(&_used_memory, __n);//记录使用了多少内存？
	ATOM_FINC(&_memory_block);//使用多少块，记录量和次数
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		ATOM_FADD(allocated, __n);
	}
	else
	{
		//错误情况
	}
	
}

inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	ATOM_FSUB(&_used_memory, __n);
	ATOM_FDEC(&_memory_block);
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		ATOM_FSUB(allocated, __n);
	}
	else
	{
		//错误情况
	}
}
//添加申请记录
inline static void*
fill_prefix(char* ptr) {
	uint32_t handle = skynet_current_handle();
	size_t size = je_malloc_usable_size(ptr);
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie));
	memcpy(&p->handle, &handle, sizeof(handle));
#ifdef MEMORY_CHECK
	uint32_t dogtag = MEMORY_ALLOCTAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	update_xmalloc_stat_alloc(handle, size);
	return ptr;
}
//清除申请记录
inline static void*
clean_prefix(char* ptr) {
	size_t size = je_malloc_usable_size(ptr);
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie));
	uint32_t handle;
	memcpy(&handle, &p->handle, sizeof(handle));
#ifdef MEMORY_CHECK
	uint32_t dogtag;
	memcpy(&dogtag, &p->dogtag, sizeof(dogtag));
	if (dogtag == MEMORY_FREETAG) {
		fprintf(stderr, "xmalloc: double free in :%08x\n", handle);
	}
	assert(dogtag == MEMORY_ALLOCTAG);	// memory out of bounds
	dogtag = MEMORY_FREETAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	update_xmalloc_stat_free(handle, size);
	return ptr;
}
//内存申请失败的错误处理
static void malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n",
		size);
	fflush(stderr);
	abort();
}

void
memory_info_dump(const char* opts) {
	je_malloc_stats_print(0,0, opts);
}

bool
mallctl_bool(const char* name, bool* newval) {
	bool v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(bool));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	return v;
}

int
mallctl_cmd(const char* name) {
	return je_mallctl(name, NULL, NULL, NULL, 0);
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	size_t v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(size_t));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	// skynet_error(NULL, "name: %s, value: %zd\n", name, v);
	return v;
}

int
mallctl_opt(const char* name, int* newval) {
	int v = 0;
	size_t len = sizeof(v);
	if(newval) {
		int ret = je_mallctl(name, &v, &len, newval, sizeof(int));
		if(ret == 0) {
			skynet_error(NULL, "set new value(%d) for (%s) succeed\n", *newval, name);
		} else {
			skynet_error(NULL, "set new value(%d) for (%s) failed: error -> %d\n", *newval, name, ret);
		}
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}

	return v;
}

// hook : malloc, realloc, free, calloc

void *
skynet_malloc(size_t size) {
	void* ptr = je_malloc(size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);//没申请到就错误处理，就是报错
	return fill_prefix(ptr);
}

void *
skynet_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return skynet_malloc(size);

	void* rawptr = clean_prefix(ptr);
	void *newptr = je_realloc(rawptr, size+PREFIX_SIZE);
	if(!newptr) malloc_oom(size);
	return fill_prefix(newptr);
}

void
skynet_free(void *ptr) {
	if (ptr == NULL) return;
	void* rawptr = clean_prefix(ptr);
	je_free(rawptr);
}

void *
skynet_calloc(size_t nmemb,size_t size) {
	void* ptr = je_calloc(nmemb + ((PREFIX_SIZE+size-1)/size), size );
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_memalign(size_t alignment, size_t size) {
	void* ptr = je_memalign(alignment, size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_aligned_alloc(size_t alignment, size_t size) {
	void* ptr = je_aligned_alloc(alignment, size + (size_t)((PREFIX_SIZE + alignment -1) & ~(alignment-1)));
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

int
skynet_posix_memalign(void **memptr, size_t alignment, size_t size) {
	int err = je_posix_memalign(memptr, alignment, size + PREFIX_SIZE);
	if (err) malloc_oom(size);
	fill_prefix(*memptr);
	return err;
}

#else

// for skynet_lalloc use
#define raw_realloc realloc
#define raw_free free

void
memory_info_dump(const char* opts) {
	skynet_error(NULL, "No jemalloc");
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_int64 %s.", name);
	return 0;
}

int
mallctl_opt(const char* name, int* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_opt %s.", name);
	return 0;
}

bool
mallctl_bool(const char* name, bool* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_bool %s.", name);
	return 0;
}

int
mallctl_cmd(const char* name) {
	skynet_error(NULL, "No jemalloc : mallctl_cmd %s.", name);
	return 0;
}

#endif

size_t
malloc_used_memory(void) {
	return ATOM_LOAD(&_used_memory);
}

size_t
malloc_memory_block(void) {
	return ATOM_LOAD(&_memory_block);
}

void
dump_c_mem() {
	int i;
	size_t total = 0;
	skynet_error(NULL, "dump all service mem:");
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			total += data->allocated;
			skynet_error(NULL, ":%08x -> %zdkb %db", data->handle, data->allocated >> 10, (int)(data->allocated % 1024));
		}
	}
	skynet_error(NULL, "+total: %zdkb",total >> 10);
}

char *//把字符串拷贝一份
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	char * ret = skynet_malloc(sz+1);
	memcpy(ret, str, sz+1);
	return ret;
}

void *//申请nsize大小的一块内存。
skynet_lalloc(void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		raw_free(ptr);
		return NULL;
	} else {
		return raw_realloc(ptr, nsize);
	}
}

int
dump_mem_lua(lua_State *L) {//依赖lua的知识：https://so.csdn.net/so/search?q=Lua%E6%BA%90%E7%A0%81%E5%88%86%E6%9E%90&t=blog&u=initphp
	int i;
	lua_newtable(L);
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			lua_pushinteger(L, data->allocated);
			lua_rawseti(L, -2, (lua_Integer)data->handle);
		}
	}
	return 1;
}

size_t
malloc_current_memory(void) {
	uint32_t handle = skynet_current_handle();
	int i;
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle == (uint32_t)handle && data->allocated != 0) {
			return (size_t) data->allocated;//查找某个线程的缓存，这里可以用红黑树来做。
		}
	}
	return 0;
}

void
skynet_debug_memory(const char *info) {
	// for debug use
	uint32_t handle = skynet_current_handle();
	size_t mem = malloc_current_memory();
	fprintf(stderr, "[:%08x] %s %p\n", handle, info, (void *)mem);
}
