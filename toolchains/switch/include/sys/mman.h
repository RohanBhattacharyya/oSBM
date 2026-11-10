#pragma once

/* Anonymous mappings supplied by StarSwitchCompat.c. This is intentionally
 * scoped to the Switch Abseil build; libnx does not implement file mappings. */
#include <stddef.h>
#include <sys/types.h>

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)

#ifdef __cplusplus
extern "C" {
#endif
void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset);
int munmap(void* address, size_t length);
#ifdef __cplusplus
}
#endif
