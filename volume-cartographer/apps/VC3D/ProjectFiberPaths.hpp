#pragma once

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace vc3d {

// Per-project annotation folder `<project dir>/<kind>/<sanitized project file
// name>`. Shared with annotation discovery: project filenames, including their
// suffix, distinguish the folders of projects saved in the same directory.
inline std::filesystem::path projectAnnotationDirectory(
    const std::filesystem::path& projectPath,
    std::string_view kind,
    const std::filesystem::path& fallbackRoot = {})
{
    const auto root = projectPath.empty() ? fallbackRoot : projectPath.parent_path();
    if (projectPath.empty() && root.empty()) return {};
    std::string name = projectPath.empty()
        ? root.filename().string() : projectPath.filename().string();
    for (char& ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '.' && ch != '-' && ch != '_') ch = '_';
    }
    while (!name.empty() && name.front() == '_') name.erase(name.begin());
    while (!name.empty() && name.back() == '_') name.pop_back();
    return root / std::string(kind) / (name.empty() ? "project" : name);
}

inline std::filesystem::path projectFiberDirectory(
    const std::filesystem::path& projectPath,
    const std::filesystem::path& fallbackRoot = {})
{
    return projectAnnotationDirectory(projectPath, "fibers", fallbackRoot);
}

} // namespace vc3d
