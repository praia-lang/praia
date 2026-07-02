#pragma once

// Builtin Error class hierarchy + catch-coercion helpers.
//
// The hierarchy itself is defined in Praia (see `kErrorClassesSource`
// in errors.cpp) and installed as builtin globals during engine
// setup. That lets `throw TypeError("bad type")` and
// `catch (e) { e is Error }` work without any `use "..."` line.
//
// The two engines diverge in how classes carry their method bodies
// — tree-walker fills `PraiaClass::methods` from AST nodes, VM fills
// `PraiaClass::vmMethods` from compiled bytecode — so the bootstrap
// runs once per engine at construction time, populating whichever
// map the running engine needs. See `bootstrapErrorClasses` overloads
// below.
//
// The C++ side of the story is `wrapRuntimeErrorForInterpreter` /
// `wrapRuntimeErrorForVm`, which sit at every catch coercion site
// that used to do `Value(std::string(err.what()))`. They look up the
// `Error` class from globals and construct a `PraiaInstance` with
// message/type/line/column fields populated directly — bypassing the
// class's Praia-side init because at catch time we might not have
// access to a live method-dispatch path in the VM.

#include "value.h"

#include <memory>
#include <string>

class Interpreter;
class Environment;
class VM;

namespace praia {

// The Praia source that defines Error + all subclasses. Kept as a
// string constant so both engines can lex/parse/execute it via their
// normal machinery.
extern const char* kErrorClassesSource;

// Run the bootstrap source once at engine construction. Each overload
// is a no-op if the source fails to compile (defensive — an engine
// with no Error class still works, catch just falls back to strings).
void bootstrapErrorClasses(Interpreter& interp);
void bootstrapErrorClasses(VM& vm);

// Construct a PraiaInstance of the named class carrying the given
// message + location. Fields are populated directly (message, type,
// line, column) without calling the class's Praia-side init — that
// keeps the helper cheap and callable from any C++ throw site.
//
// `className` typically matches the class registered by the
// bootstrap (Error, TypeError, IOError, …). If the class isn't in
// globals — e.g. the bootstrap failed — the helper falls back to a
// bare string Value so the catch handler still sees *something*.
Value makeErrorInstance(const Value& classValue,
                        const std::string& className,
                        const std::string& message,
                        int line, int column);

// Engine-specific class lookups. Return the class Value if found in
// globals; nil Value otherwise (caller falls back to string coercion).
Value lookupErrorClass(Interpreter& interp, const std::string& className);
Value lookupErrorClass(VM& vm, const std::string& className);

// Shared catch-coercion helpers used at every catch site that used
// to do `Value(std::string(err.what()))`. Returns the caught-error
// value that gets bound to the user's catch variable.
Value wrapRuntimeErrorForInterpreter(Interpreter& interp,
                                     const RuntimeError& re);
Value wrapRuntimeErrorForVm(VM& vm, const RuntimeError& re);

} // namespace praia
