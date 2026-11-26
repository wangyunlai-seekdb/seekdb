#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>
#endif

#define OB_NOINLINE __attribute__((noinline))
#define _RLOCAL(type, var) thread_local type var
#define RLOCAL_EXTERN(type, var) extern thread_local type var
#define STATIC_ASSERT(...) static_assert(__VA_ARGS__)
#define OB_ISNULL(expr) ((expr) == nullptr)
#define OB_LIKELY(expr) (expr)
#define OB_UNLIKELY(expr) (expr)
#define FALSE_IT(expr) ((void)(expr), false)
#define COMMON_LOG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_ERROR_RET(...) ((void)0)
#define LOG_WARN_RET(...) ((void)0)
#define abort_unless(expr) do { if (!(expr)) std::abort(); } while (false)
#define CONCAT_DETAIL(a, b) a ## b
#define CONCAT(a, b) CONCAT_DETAIL(a, b)
#define DEFER(...) auto CONCAT(scope_exit_, __COUNTER__) = ScopeExit{[&] { __VA_ARGS__; }}

template <typename F> struct ScopeExit { F run; ~ScopeExit() { run(); } };
template <typename F> ScopeExit(F) -> ScopeExit<F>;

struct ObCtxIds { static constexpr int CO_STACK = 0, DEFAULT_CTX_ID = 1; };
struct ObMemAttr { ObMemAttr(const char *, int) {} };
inline thread_local int allocations = 0;
inline thread_local int releases = 0;
inline void *ob_malloc(size_t length, ObMemAttr) {
  void *address = std::malloc(length);
  if (address) ++allocations;
  return address;
}
inline void ob_free(void *address) { ++releases; std::free(address); }
inline bool &in_hook() { static thread_local bool active = false; return active; }
inline size_t get_page_size() {
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}
inline uint64_t align_up2(uint64_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
