/* arch/cc.h : lwIP's platform glue for Linux userspace (aarch64 and x86_64, little endian). */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwip assert: %s at %s:%d\n", x, __FILE__, __LINE__); fflush(NULL); abort(); } while (0)
#define LWIP_RAND() ((u32_t)random())
#define lwip_htons(x) ((u16_t)__builtin_bswap16((u16_t)(x)))
#define lwip_htonl(x) ((u32_t)__builtin_bswap32((u32_t)(x)))
#define LWIP_NO_CTYPE_H 0
#endif
