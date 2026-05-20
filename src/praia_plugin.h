// Praia Native Plugin API
//
// Include this single header in your plugin source file.
// Your plugin must export:
//
//   extern "C" void praia_register(PraiaMap* module);
//
// Example:
//
//   #include "praia_plugin.h"
//
//   extern "C" void praia_register(PraiaMap* module) {
//       module->entries["double"] = Value(makeNative("mymod.double", 1,
//           [](const std::vector<Value>& args) -> Value {
//               if (!args[0].isNumber())
//                   throw RuntimeError("expected a number", 0);
//               return Value(args[0].asNumber() * 2);
//           }));
//   }
//
// Build:
//   make plugin SRC=myplugin.cpp OUT=myplugin.dylib   # macOS
//   make plugin SRC=myplugin.cpp OUT=myplugin.so      # Linux
//
// Use in Praia:
//   let mymod = loadNative("./myplugin")  // extension auto-detected
//   print(mymod.double(21))  // 42

#pragma once

#include "value.h"          // Value, PraiaArray, PraiaMap, RuntimeError
#include "gc_heap.h"        // gcNew<T>()
#include "builtins.h"       // makeNative()
#include "praia_runtime.h"  // praia::currentExecutor, praia::invokeExecutor

namespace praia {

// Invoke a Praia Callable received as a plugin argument. Use this
// to call back into user code (e.g. when the plugin accepts a
// lambda for a filter/sort/iterator-style API).
//
//   module->entries["forEach"] = Value(makeNative("mymod.forEach", 2,
//       [](const std::vector<Value>& args) -> Value {
//           auto& arr = args[0].asArray()->elements;
//           auto fn   = args[1].asCallable();
//           for (auto& v : arr) praia::call(fn, {v});
//           return Value();
//       }));
//
// Pitfall — deferred invocation. Both engines stash their current
// pointer in a thread-local that's only valid for the duration of
// the active call frame. If a plugin stashes a Callable and
// invokes it later from a worker thread or libuv-style callback,
// `praia::call` throws because no engine is bound to that thread.
// Callbacks must be invoked synchronously from the same plugin
// thread that received them, or the plugin must marshal back to
// the engine thread first.
inline Value call(const std::shared_ptr<Callable>& fn,
                  const std::vector<Value>& args = {}) {
    void* exec = currentExecutor();
    if (!exec) {
        throw RuntimeError(
            "praia::call invoked with no active Praia executor on this thread", 0);
    }
    return invokeExecutor(exec, fn, args);
}

}  // namespace praia
