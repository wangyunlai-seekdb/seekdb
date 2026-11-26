#include "stack.h"

using namespace oceanbase::common;

void touch_frame(volatile char *, size_t);

void verify(bool result, const char *failure) {
  if (!result) {
    std::fprintf(stderr, "FAIL %s\n", failure);
    std::abort();
  }
}

struct Bounds {
  void *base = nullptr;
  size_t size = 0;

  bool contains(const volatile void *pointer) const {
    auto addr = reinterpret_cast<uintptr_t>(pointer);
    auto low = reinterpret_cast<uintptr_t>(base);
    return low <= addr && addr - low < size;
  }
};

Bounds bounds() {
  Bounds result;
  verify(get_stackattr(result.base, result.size) == OB_SUCCESS, "platform stack bounds");
  return result;
}

struct State { int switches = 0; int restores = 0; };

int descend(State &state);

OB_NOINLINE int on_alternate(State &state, Bounds previous) {
  volatile char here = 0;
  auto current = bounds();
  verify(current.base != previous.base && current.size == STACK_PER_EXTEND &&
      current.contains(&here) && !previous.contains(&here), "callback moved to alternate stack");
#ifdef _WIN32
  auto *tib = reinterpret_cast<NT_TIB *>(NtCurrentTeb());
  verify(tib->StackLimit == current.base && tib->StackBase == static_cast<char *>(current.base) + current.size,
      "Windows TIB tracks alternate stack");
#endif
  ++state.switches;
  return descend(state);
}

OB_NOINLINE int descend(State &state) {
  volatile char frame[8192];
  touch_frame(frame, sizeof(frame));
  bool short_on_stack = false;
  verify(check_stack_overflow(short_on_stack, STACK_RESERVED_SIZE) == OB_SUCCESS, "stack budget probe");
  int result;
  if (short_on_stack) {
    auto previous = bounds();
#ifdef _WIN32
    auto *tib = reinterpret_cast<NT_TIB *>(NtCurrentTeb());
    void *original_base = tib->StackBase;
    void *original_limit = tib->StackLimit;
#endif
    int before = state.switches;
    result = SMART_CALL(on_alternate(state, previous));
    auto restored = bounds();
    verify(restored.base == previous.base && restored.size == previous.size &&
        restored.contains(&frame[0]), "parent stack and cached bounds restored");
#ifdef _WIN32
    verify(tib->StackBase == original_base && tib->StackLimit == original_limit, "Windows TIB restored");
#endif
    if (state.switches != before) ++state.restores;
  } else {
    result = descend(state);
  }
  verify(frame[0] == 7 && frame[sizeof(frame)-1] == 11, "parent frame intact");
  return result;
}

void exercise() {
  volatile char here = 0;
  auto initial = bounds();
  verify(initial.contains(&here), "actual worker address in detected stack range");
  int calls = 0;
  int normal = SMART_CALL((++calls, 37));
  verify(normal == 37 && calls == 1 && allocations == 0, "normal path callback returns without allocating");
  State state;
  int result = descend(state);
#ifdef __EMSCRIPTEN__
  int expected = 0;
#else
  const size_t limit = ALL_STACK_LIMIT;
  int expected = initial.size < limit ? (limit - initial.size) / STACK_PER_EXTEND : 0;
#endif
  verify(result == OB_SIZE_OVERFLOW && state.switches == expected && state.restores == expected,
      "nested calls restore their old stacks and stop at the configured limit");
  verify(allocations == expected && releases == expected, "alternate allocator balances successful switches");
  std::printf("PASS worker_stack_bytes=%zu callback_switches=%d restored=%d alloc=%d free=%d limit_rc=%d\n",
      initial.size, state.switches, state.restores, allocations, releases, result);
  std::fflush(stdout);
}

#ifdef _WIN32
DWORD WINAPI worker(LPVOID) { exercise(); return 0; }
#else
void *worker(void *) { exercise(); return nullptr; }
#endif

int main(int argc, char **argv) {
  const size_t worker_stack_size = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 512 * 1024;
#ifdef _WIN32
  HANDLE thread = CreateThread(nullptr, worker_stack_size, worker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  verify(thread && WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0, "create and join Windows worker");
  CloseHandle(thread);
#else
  pthread_attr_t attr;
  pthread_t thread;
  verify(pthread_attr_init(&attr) == 0, "pthread_attr_init");
  verify(pthread_attr_setstacksize(&attr, worker_stack_size) == 0, "pthread_attr_setstacksize");
  verify(pthread_create(&thread, &attr, worker, nullptr) == 0, "pthread_create");
  verify(pthread_attr_destroy(&attr) == 0 && pthread_join(thread, nullptr) == 0, "pthread_join");
#endif
  return 0;
}
