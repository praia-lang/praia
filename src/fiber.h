#pragma once

// Portable fiber (stackful coroutine) using ucontext_t.
// Provides cooperative context switching for generator implementation.

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <ucontext.h>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <sys/mman.h>

#include "stack_guard.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

// ASan tracks one stack per thread: each switch onto or off a fiber's
// stack has to tell it, or it misreads the frames there
#if defined(__SANITIZE_ADDRESS__)
#define PRAIA_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PRAIA_ASAN 1
#endif
#endif

#ifdef PRAIA_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

class Fiber {
public:
    // As much as a main thread gets: the tree-walker uses several KB of
    // C++ stack per Praia call (tens of KB in a debug build). Only the
    // pages it touches take memory
    static constexpr size_t DEFAULT_STACK_SIZE = 8 * 1024 * 1024;

    Fiber(std::function<void()> body, size_t stackSize = DEFAULT_STACK_SIZE)
        : body_(std::move(body)), stackSize_(stackSize) {
        // Allocate stack with mmap for proper alignment and guard page potential
        stack_ = static_cast<uint8_t*>(
            mmap(nullptr, stackSize_, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        if (stack_ == MAP_FAILED) {
            stack_ = nullptr;
            return;
        }

        getcontext(&fiberCtx_);
#if defined(__APPLE__)
        // makecontext here zeroes all the stack it's given, which for one
        // this size costs far more than a generator does. It's given the
        // top: the fiber starts there and grows down into the rest of the
        // mapping, which the stack checks measure (see resume)
        const size_t contextStack = std::min<size_t>(64 * 1024, stackSize_);
        fiberCtx_.uc_stack.ss_sp = stack_ + stackSize_ - contextStack;
        fiberCtx_.uc_stack.ss_size = contextStack;
#else
        fiberCtx_.uc_stack.ss_sp = stack_;
        fiberCtx_.uc_stack.ss_size = stackSize_;
#endif
        fiberCtx_.uc_link = nullptr;

        // makecontext only accepts int args; split the Fiber* into two halves
        auto ptr = reinterpret_cast<uintptr_t>(this);
        auto lo = static_cast<unsigned int>(ptr & 0xFFFFFFFF);
        auto hi = static_cast<unsigned int>((ptr >> 32) & 0xFFFFFFFF);
        makecontext(&fiberCtx_, reinterpret_cast<void(*)()>(fiberEntry), 2, lo, hi);
    }

    ~Fiber() {
        if (!stack_) return;
#ifdef PRAIA_ASAN
        // A generator dropped while suspended never unwinds, leaving its
        // frames' redzones poisoned for whatever is mapped here next
        __asan_unpoison_memory_region(stack_, stackSize_);
#endif
        munmap(stack_, stackSize_);
    }

    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    void resume() {
        if (completed_ || !stack_) return;
        Fiber* prev = current_;
        current_ = this;
        // The stack checks measure against this fiber's stack while it runs
        praia::StackBounds prevStack = praia::fiberStack;
        praia::fiberStack = {reinterpret_cast<uintptr_t>(stack_), stackSize_};
        void* fake = nullptr;
        switchStart(&fake, stack_, stackSize_);
        swapcontext(&callerCtx_, &fiberCtx_);
        switchFinish(fake, nullptr, nullptr);
        praia::fiberStack = prevStack;
        current_ = prev;
    }

    static void suspend() {
        Fiber* self = current_;
        void* fake = nullptr;
        switchStart(&fake, self->callerBottom_, self->callerSize_);
        swapcontext(&self->fiberCtx_, &self->callerCtx_);
        switchFinish(fake, &self->callerBottom_, &self->callerSize_);
    }

    bool isCompleted() const { return completed_; }

    // Runs body to completion on a fiber's stack and returns what it
    // returns (or rethrows what it throws): gives a thread with a small
    // stack, like an async task's (512 KB on macOS), a main thread's room.
    // Runs it in place if the stack can't be mapped
    template <class T, class F>
    static T run(F&& body) {
        T result;
        std::exception_ptr error;
        Fiber fiber([&] {
            try { result = body(); }
            catch (...) { error = std::current_exception(); }
        });
        if (!fiber.stack_) return body();
        fiber.resume();
        if (error) std::rethrow_exception(error);
        return result;
    }

private:
    static void fiberEntry(unsigned int lo, unsigned int hi) {
        auto ptr = static_cast<uintptr_t>(lo) | (static_cast<uintptr_t>(hi) << 32);
        Fiber* self = reinterpret_cast<Fiber*>(ptr);
        switchFinish(nullptr, &self->callerBottom_, &self->callerSize_);
        self->body_();
        self->completed_ = true;
        // No fake stack to save: this fiber never runs again
        switchStart(nullptr, self->callerBottom_, self->callerSize_);
        swapcontext(&self->fiberCtx_, &self->callerCtx_);
    }

    // Tell ASan about a switch to the stack at bottom, then that it's
    // done, learning the stack switched from. No-ops in a normal build
    static void switchStart(void** fake, const void* bottom, size_t size) {
#ifdef PRAIA_ASAN
        __sanitizer_start_switch_fiber(fake, bottom, size);
#else
        (void)fake; (void)bottom; (void)size;
#endif
    }
    static void switchFinish(void* fake, const void** bottom, size_t* size) {
#ifdef PRAIA_ASAN
        __sanitizer_finish_switch_fiber(fake, bottom, size);
#else
        (void)fake; (void)bottom; (void)size;
#endif
    }

    // The stack this fiber was last switched in from, for switching back
    const void* callerBottom_ = nullptr;
    size_t callerSize_ = 0;

    ucontext_t callerCtx_{};
    ucontext_t fiberCtx_{};
    std::function<void()> body_;
    uint8_t* stack_ = nullptr;
    size_t stackSize_;
    bool completed_ = false;

    static thread_local Fiber* current_;
};
