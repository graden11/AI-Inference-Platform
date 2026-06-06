#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace http {
namespace utils {

namespace fs = std::filesystem;

/// Returns true if `filePath` resolves to a location inside `allowedDir`
/// (or inside any of the directories in `allowedDirs`).
/// Uses filesystem canonicalization to defeat ../ and symlink traversal.
inline bool isPathSafeInDir(const std::string& filePath,
                            const std::string& allowedDir)
{
    std::error_code ec;
    fs::path target = fs::weakly_canonical(filePath, ec);
    if (ec) return false;
    fs::path root = fs::weakly_canonical(allowedDir, ec);
    if (ec) return false;
    fs::path rel = fs::relative(target, root, ec);
    if (ec) return false;
    if (rel.empty()) return false;
    if (rel.begin()->string() == "..") return false;
    if (rel.is_absolute()) return false;
    return true;
}

/// Overload that checks against multiple allowed directories.
inline bool isPathSafeInDirs(const std::string& filePath,
                             const std::vector<std::string>& allowedDirs)
{
    for (const auto& dir : allowedDirs) {
        if (isPathSafeInDir(filePath, dir))
            return true;
    }
    return false;
}

} // namespace utils
} // namespace http
