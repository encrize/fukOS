#ifndef _DG_ASSERT_H
#define _DG_ASSERT_H
void __dg_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __dg_assert_fail(#x, __FILE__, __LINE__))
#endif
#endif
