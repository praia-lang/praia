#pragma once

// How much C++ stack is left. Deep Praia code and data turn into deep C++
// recursion: the tree-walker evaluating a long expression or call chain,
// the parser and compiler descending into nested code, natives calling
// back into the VM, printing or copying a deeply nested value. A count of
// calls can't say when that runs out: frames are bigger in a debug build,
// and VM async tasks run on 512 KB thread stacks (macOS). So these places
// ask how close the stack itself is to its end, and stop with an error
// while a reserve is left.

#include <pthread.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace praia {

struct StackBounds {
    uintptr_t low = 0;   // lowest usable address (stacks grow down)
    size_t size = 0;     // 0 = unknown, never reported as full
};

// The running generator fiber's stack, while one runs on this thread
// (set by Fiber::resume)
inline thread_local StackBounds fiberStack;

inline StackBounds threadStackBounds() {
    StackBounds b;
#if defined(__APPLE__)
    pthread_t self = pthread_self();
    auto top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
    b.size = pthread_get_stacksize_np(self);
    b.low = top - b.size;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
            b.low = reinterpret_cast<uintptr_t>(addr);
            b.size = size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
    return b;
}

// True once less than the reserve is left: a quarter of the stack,
// between 64 KB and 256 KB. Enough for the deepest stretch of C++
// between two checks, and for unwinding and reporting the error.
inline bool stackNearlyFull() {
    thread_local const StackBounds threadStack = threadStackBounds();
    const StackBounds& b = fiberStack.size ? fiberStack : threadStack;
    if (!b.size) return false;
    size_t reserve = std::clamp<size_t>(b.size / 4, 64 * 1024, 256 * 1024);
    auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    return here < b.low + reserve;
}

} // namespace praia
