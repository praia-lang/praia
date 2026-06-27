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
    if (warnedSet.insert(oldName).second) {
        std::cerr << formatLocation(line, column)
                  << " Warning: " << msg << std::endl;
    }
}

} // namespace praia
