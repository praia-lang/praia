#pragma once

#include <atomic>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

struct Callable;

// Shared signal state — used by interpreter_setup.cpp (registration),
// interpreter.cpp (tree-walker SIGINT check), and vm.cpp (VM SIGINT check).

extern std::mutex g_signalMutex;
extern std::unordered_map<int, std::shared_ptr<Callable>> g_signalHandlers;
extern std::atomic<uint32_t> g_pendingSignals;

// The C signal handler — async-signal-safe (only sets atomic flag)
void praiaSignalHandler(int sig);

// Install the default SIGINT handler (call once at startup)
void installDefaultSignalHandlers();
