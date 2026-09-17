/* Платформенные определения LwIP для arm-none-eabi-gcc. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdlib.h>

#define LWIP_NO_STDINT_H 0
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_LIMITS_H 0
#define LWIP_NO_CTYPE_H 0
#define LWIP_NO_UNISTD_H 1

#define BYTE_ORDER LITTLE_ENDIAN

#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "u"

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#define LWIP_PLATFORM_DIAG(x)
#define LWIP_PLATFORM_ASSERT(x) \
    do {                        \
        for (;;) {              \
        }                       \
    } while (0)

#endif /* LWIP_ARCH_CC_H */
