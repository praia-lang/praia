#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// Directory where the praia binary lives — set once from main().
// Used to find bundled stdlib grains in development mode: <bindir>/grains/
inline std::string g_praiaInstallDir;

// Full canonical path to the praia binary itself (e.g. "/usr/local/bin/praia").
// Set alongside g_praiaInstallDir from main(). Used by `praia test` to fork+exec
// itself to run each test file in an isolated subprocess.
inline std::string g_praiaExecPath;

// Compile-time LIBDIR from make install (e.g. "/usr/local/lib/praia").
// When set, grains are found at PRAIA_LIBDIR/grains/.
// When empty, falls back to g_praiaInstallDir-relative resolution.
#ifdef PRAIA_LIBDIR
inline const char* g_praiaLibDir = PRAIA_LIBDIR;
#else
inline const char* g_praiaLibDir = nullptr;
#endif

// Verify that a resolved path stays within an expected base directory.
// Prevents symlink escapes (e.g. ext_grains/evil -> /etc/passwd).
// Returns "" on rejection (containment escaped) OR on filesystem error
// (missing file, permission denied) — both treated as "not a valid grain".
inline std::string containedCanonical(const fs::path& file, const fs::path& base) {
    std::error_code ec;
    auto resolved = fs::canonical(file, ec).string();
    if (ec) return "";
    auto baseCanonical = fs::canonical(base, ec).string();
    if (ec) return "";
    // Ensure resolved path starts with base + separator (or equals base)
    if (resolved.rfind(baseCanonical, 0) != 0)
        return ""; // escaped containment
    // Must be exactly base or base/...
    if (resolved.size() > baseCanonical.size() && resolved[baseCanonical.size()] != '/')
        return ""; // e.g. base="/foo/bar", resolved="/foo/barBaz"
    return resolved;
}

// Check if a directory is a grain with a grain.yaml.
// Returns the resolved entry file path, or empty string if not a grain directory.
inline std::string resolveGrainDir(const fs::path& dir, const fs::path& base) {
    if (!fs::is_directory(dir)) return "";

    // Look for grain.yaml to find the main entry file
    auto manifest = dir / "grain.yaml";
    if (fs::exists(manifest)) {
        std::ifstream f(manifest);
        std::string line;
        while (std::getline(f, line)) {
            // Simple YAML parse: look for "main: <filename>"
            if (line.starts_with("main:")) {
                std::string mainFile = line.substr(5);
                // Trim whitespace
                size_t start = mainFile.find_first_not_of(" \t");
                if (start != std::string::npos) mainFile = mainFile.substr(start);
                size_t end = mainFile.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) mainFile = mainFile.substr(0, end + 1);
                if (!mainFile.empty()) {
                    auto entry = dir / mainFile;
                    if (fs::exists(entry)) return containedCanonical(entry, base);
                }
            }
        }
    }

    // Fallback: main.praia, then index.praia
    auto mainFile = dir / "main.praia";
    if (fs::exists(mainFile)) return containedCanonical(mainFile, base);

    auto indexFile = dir / "index.praia";
    if (fs::exists(indexFile)) return containedCanonical(indexFile, base);

    return "";
}

// Try to resolve a grain name in a given base directory.
// Checks <base>/<name>.praia first, then <base>/<name>/ as a directory grain.
inline std::string tryResolveGrain(const fs::path& base, const std::string& name) {
    // Single file
    auto singleFile = base / (name + ".praia");
    if (fs::exists(singleFile)) return containedCanonical(singleFile, base);

    // Directory grain
    auto grainDir = base / name;
    return resolveGrainDir(grainDir, base);
}

// Resolve a grain import path to an absolute file path.
// Searches (in order): relative paths, ext_grains/, grains/,
// ~/.praia/ext_grains/, LIBDIR/ext_grains/, LIBDIR/grains/.
// Throws std::runtime_error if the grain cannot be found.
inline std::string resolveGrainPath(const std::string& importPath,
                                     const std::string& currentFile) {
    // 1. Relative path (starts with ./ or ../)
    if (importPath.starts_with("./") || importPath.starts_with("../")) {
        fs::path baseDir = currentFile.empty() ? fs::current_path()
                                                : fs::path(currentFile).parent_path();
        fs::path resolved = baseDir / (importPath + ".praia");
        if (!fs::exists(resolved))
            throw std::runtime_error("Grain not found: " + importPath +
                                      " (looked in " + resolved.string() + ")");
        // Reject if the file at the resolved path is itself a symlink — the
        // user wrote a literal relative path, but the file redirects through
        // a symlink to somewhere else. Defense-in-depth for hosts that load
        // untrusted grains. (Symlinks in parent directories are still allowed.)
        std::error_code ec;
        if (fs::is_symlink(resolved, ec))
            throw std::runtime_error("Grain '" + importPath +
                                      "' resolves to a symlink (refusing to load)");
        auto canon = fs::canonical(resolved, ec);
        if (ec)
            throw std::runtime_error("Grain not found: " + importPath +
                                      " (" + ec.message() + ")");
        return canon.string();
    }

    // 2. ext_grains/ (local dependencies installed by sand)
    if (!currentFile.empty()) {
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) {
            auto r = tryResolveGrain(dir / "ext_grains", importPath);
            if (!r.empty()) return r;
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
    {
        auto r = tryResolveGrain(fs::current_path() / "ext_grains", importPath);
        if (!r.empty()) return r;
    }

    // 3. grains/ directory (project-level, bundled grains)
    if (!currentFile.empty()) {
        fs::path dir = fs::path(currentFile).parent_path();
        for (int i = 0; i < 10; i++) {
            auto r = tryResolveGrain(dir / "grains", importPath);
            if (!r.empty()) return r;
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
    {
        auto r = tryResolveGrain(fs::current_path() / "grains", importPath);
        if (!r.empty()) return r;
    }

    // 4. ~/.praia/ext_grains/ (user-global)
    {
        const char* home = std::getenv("HOME");
        if (home) {
            auto r = tryResolveGrain(fs::path(home) / ".praia" / "ext_grains", importPath);
            if (!r.empty()) return r;
        }
    }

    // 5. Bundled stdlib grains + system-global ext_grains
    if (g_praiaLibDir) {
        auto r = tryResolveGrain(fs::path(g_praiaLibDir) / "ext_grains", importPath);
        if (!r.empty()) return r;
        r = tryResolveGrain(fs::path(g_praiaLibDir) / "grains", importPath);
        if (!r.empty()) return r;
    } else if (!g_praiaInstallDir.empty()) {
        auto r = tryResolveGrain(fs::path(g_praiaInstallDir) / "grains", importPath);
        if (!r.empty()) return r;
    }

    throw std::runtime_error("Grain not found: " + importPath);
}
