// asan.hpp — AddressSanitizer's fiber-switching hooks, when building with ASan.
//
// ASan keeps a shadow of the stack and a per-thread notion of where the stack
// is. A hand-made stack switch confuses both unless the program announces it:
// start_switch_fiber before leaving a stack, finish_switch_fiber on arrival.
// Without this, a sanitized build reports false stack-buffer errors or crashes
// in the sanitizer runtime.
#pragma once

#include <cstddef>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FIB_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define FIB_ASAN 1
#endif

#ifdef FIB_ASAN
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, std::size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, std::size_t* size_old);
}
#endif
