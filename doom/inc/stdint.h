#ifndef _DG_STDINT_H
#define _DG_STDINT_H
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int_least8_t;
typedef short              int_least16_t;
typedef int                int_least32_t;
typedef long long          int_least64_t;
typedef unsigned char      uint_least8_t;
typedef unsigned short     uint_least16_t;
typedef unsigned int       uint_least32_t;
typedef unsigned long long uint_least64_t;
typedef signed char        int_fast8_t;
typedef int                int_fast16_t;
typedef int                int_fast32_t;
typedef long long          int_fast64_t;
typedef unsigned char      uint_fast8_t;
typedef unsigned int       uint_fast16_t;
typedef unsigned int       uint_fast32_t;
typedef unsigned long long uint_fast64_t;
typedef int                intptr_t;
typedef unsigned int       uintptr_t;
typedef long long          intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_MIN   (-128)
#define INT16_MIN  (-32767-1)
#define INT32_MIN  (-2147483647-1)
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define INT64_MAX  9223372036854775807LL
#define INT64_MIN  (-INT64_MAX-1)
#define UINT64_MAX 18446744073709551615ULL
#define INTPTR_MIN INT32_MIN
#define INTPTR_MAX INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#define SIZE_MAX   UINT32_MAX

#define INT8_C(x)   x
#define INT16_C(x)  x
#define INT32_C(x)  x
#define INT64_C(x)  x##LL
#define UINT8_C(x)  x
#define UINT16_C(x) x
#define UINT32_C(x) x##U
#define UINT64_C(x) x##ULL
#endif
