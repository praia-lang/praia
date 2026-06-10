#pragma once

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace praia {

// Bounded Levenshtein distance between `a` and `b`. Returns
// std::numeric_limits<int>::max() when either string exceeds `maxLen`,
// so a pathologically long identifier in user code can't blow up the
// O(n·m) DP table. 64 is comfortably above any real identifier
// length and keeps the table at 64*64*sizeof(int) ≈ 16 KiB worst case.
inline int editDistance(const std::string& a, const std::string& b, int maxLen = 64) {
    int n = static_cast<int>(a.size());
    int m = static_cast<int>(b.size());
    if (n > maxLen || m > maxLen) return std::numeric_limits<int>::max();
    if (n == 0) return m;
    if (m == 0) return n;

    // Two-row DP saves memory; we only need the previous row.
    std::vector<int> prev(m + 1), curr(m + 1);
    for (int j = 0; j <= m; j++) prev[j] = j;

    for (int i = 1; i <= n; i++) {
        curr[0] = i;
        for (int j = 1; j <= m; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({
                prev[j] + 1,        // deletion
                curr[j - 1] + 1,    // insertion
                prev[j - 1] + cost  // substitution
            });
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

// Threshold for "close enough to suggest as a typo." Tight enough that
// short names like `Ok` don't fuzzy-match unrelated short names
// (`On`, `Or`) — those are real risks in dense codebases — but loose
// enough that `Deqeu` matches `Deque` and `MyServie` matches `MyService`.
inline int suggestionThreshold(const std::string& name) {
    return name.size() <= 3 ? 1 : 2;
}

// Scan `callables` for a name within suggestionThreshold(name) edits.
// Returns the first match (not necessarily the closest) — at the
// warning level we don't need a ranked list, just one plausible
// suggestion to point at. Names equal to `name` are skipped because
// the caller already checked that `name` itself didn't resolve.
template <class Range>
inline std::optional<std::string> closestCallable(const std::string& name,
                                                  const Range& callables) {
    int threshold = suggestionThreshold(name);
    for (const auto& candidate : callables) {
        if (candidate == name) continue;
        // Cheap length pre-filter — if the size delta already exceeds
        // the threshold, edit distance is at least that big.
        int delta = static_cast<int>(candidate.size()) - static_cast<int>(name.size());
        if (delta < 0) delta = -delta;
        if (delta > threshold) continue;
        if (editDistance(name, candidate) <= threshold) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace praia
