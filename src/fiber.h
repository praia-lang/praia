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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <sys/mman.h>

#include "stack_guard.h"

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

class Fiber {
public:
    static constexpr size_t DEFAULT_STACK_SIZE = 262144; // 256KB

    Fiber(std::function<void()> body, size_t stackSize = DEFAULT_STACK_SIZE)
        : body_(std::move(body)), stackSize_(stackSize) {
        // Allocate stack with mmap for proper alignment and guard page potential
        stack_ = static_cast<uint8_t*>(
            mmap(nullptr, stackSize_, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (stack_ == MAP_FAILED) {
            stack_ = nullptr;
            return;
        }

        getcontext(&fiberCtx_);
        fiberCtx_.uc_stack.ss_sp = stack_;
        fiberCtx_.uc_stack.ss_size = stackSize_;
        fiberCtx_.uc_link = nullptr;

        // makecontext only accepts int args; split the Fiber* into two halves
        auto ptr = reinterpret_cast<uintptr_t>(this);
        auto lo = static_cast<unsigned int>(ptr & 0xFFFFFFFF);
        auto hi = static_cast<unsigned int>((ptr >> 32) & 0xFFFFFFFF);
        makecontext(&fiberCtx_, reinterpret_cast<void(*)()>(fiberEntry), 2, lo, hi);
    }

    ~Fiber() {
        if (stack_) munmap(stack_, stackSize_);
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
        swapcontext(&callerCtx_, &fiberCtx_);
        praia::fiberStack = prevStack;
        current_ = prev;
    }

    static void suspend() {
        Fiber* self = current_;
        swapcontext(&self->fiberCtx_, &self->callerCtx_);
    }

    bool isCompleted() const { return completed_; }

private:
    static void fiberEntry(unsigned int lo, unsigned int hi) {
        auto ptr = static_cast<uintptr_t>(lo) | (static_cast<uintptr_t>(hi) << 32);
        Fiber* self = reinterpret_cast<Fiber*>(ptr);
        self->body_();
        self->completed_ = true;
        swapcontext(&self->fiberCtx_, &self->callerCtx_);
    }

    ucontext_t callerCtx_{};
    ucontext_t fiberCtx_{};
    std::function<void()> body_;
    uint8_t* stack_ = nullptr;
    size_t stackSize_;
    bool completed_ = false;

    static thread_local Fiber* current_;
};
