#include <cstddef>
void touch_frame(volatile char *frame, size_t size) {
  for (size_t offset = 0; offset < size; offset += 4096) frame[offset] = 7;
  frame[size - 1] = 11;
}
