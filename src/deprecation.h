#pragma once

#include "value.h"

#include <iostream>
#include <string>
#include <unordered_set>

namespace praia {

// Emit (or fail on) a deprecation warning for a renamed method.
//
// `strict`        — the engine's `--strict-deprecations` flag state.
// `warnedSet`     — per-engine dedup set; insertion is the gate that
//                   prevents the same name warning more than once
//                   per run (mirrors warnedTagNames_ from the
//                   --strict-tags work).
// `oldName`       — the deprecated method name (e.g. "sort").
// `replacementPure` / `replacementMutating` — the two new names to
//                   point the user at, so they can pick the
//                   semantics they actually wanted.
// `line`/`column` — source location of the call site for the
//                   diagnostic.
//
// Behaviour: under `strict`, throws `RuntimeError` immediately so a
// CI-gated build can hard-fail on legacy names. Otherwise emits a
// one-line stderr warning the first time `oldName` is seen this run
// and a no-op on subsequent calls.
inline void emitMethodDeprecation(
        bool strict,
        std::unordered_set<std::string>& warnedSet,
        const std::string& oldName,
        const std::string& replacementPure,
        const std::string& replacementMutating,
        int line, int column = 0) {
    std::string msg = "'." + oldName + "()' is deprecated; use '."
        + replacementPure + "()' (pure) or '."
        + replacementMutating + "()' (mutating)";
    if (strict) {
        throw RuntimeError("Strict mode: " + msg, line, column);
    }
    // Namespace the dedup key so a method-rename warning for
    // `arr.sort()` can't be silently suppressed by an unrelated
    // `sys.notifyDeprecation("sort", ...)` (or vice versa). The set
    // is shared across both call paths; the prefix keeps the
    // categories distinct.
    if (warnedSet.insert("method:" + oldName).second) {
        std::cerr << formatLocation(line, column)
                  << " Warning: " << msg << std::endl;
    }
}

// Sibling helper for grain-level / general-purpose deprecations.
// Same dedup + strict-mode behaviour as `emitMethodDeprecation`,
// but takes a single freeform `hint` string instead of the two
// replacement-method names — grain-class deprecations carry richer
// migration text than the method-rename pattern can express.
//
// Surfaced to user code via `sys.notifyDeprecation(name, hint)`,
// so stdlib grains (and any future user-written deprecation) can
// piggyback on the same warned-set dedup and `--strict-deprecations`
// CI gate the C++ helper already provides.
inline void emitNamedDeprecation(
        bool strict,
        std::unordered_set<std::string>& warnedSet,
        const std::string& name,
        const std::string& hint,
        int line, int column = 0) {
    std::string msg = "'" + name + "' is deprecated; " + hint;
    if (strict) {
        throw RuntimeError("Strict mode: " + msg, line, column);
    }
    // Namespace the dedup key (see `emitMethodDeprecation` for the
    // matching rationale). The two emitters share `warnedSet` but
    // must not poison each other's dedup state.
    if (warnedSet.insert("named:" + name).second) {
        std::cerr << formatLocation(line, column)
                  << " Warning: " << msg << std::endl;
    }
}

} // namespace praia
