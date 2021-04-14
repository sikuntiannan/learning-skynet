#ifndef skynet_malloc_h
#define skynet_malloc_h

#include <stddef.h>

#define skynet_malloc malloc
#define skynet_calloc calloc
#define skynet_realloc realloc
#define skynet_free free
#define skynet_memalign memalign
#define skynet_aligned_alloc aligned_alloc
#define skynet_posix_memalign posix_memalign

void * skynet_malloc(size_t sz);//分配连续长度
void * skynet_calloc(size_t nmemb,size_t size);//分配nmemb*size个长度，一个是数量一个是元素大小
void * skynet_realloc(void *ptr, size_t size);//重新向ptr分配大小为size的一块内存
void skynet_free(void *ptr);//释放内存
char * skynet_strdup(const char *str);//字符串拷贝，把用户自己的字符串再来一份，用户自己的可能一会就没了。
void * skynet_lalloc(void *ptr, size_t osize, size_t nsize);	// use for lua；第二个参数没有用
void * skynet_memalign(size_t alignment, size_t size);
void * skynet_aligned_alloc(size_t alignment, size_t size);
int skynet_posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
