#pragma once

// The region files of one project: one `<id>.region.json` per region in the
// project's regions folder. Every mutation takes the folder lock, re-reads
// the target's revision from disk, writes through QSaveFile and only then
// changes memory, so a failed write never leaves memory ahead of the disk.
// Uses QtCore only (no widgets), so it is tested without a GUI.

#include "RegionModel.hpp"

#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vc3d::regions
{

enum class Code { Ok, NoProject, NoVolume, Invalid, NotFound, Conflict, ReadOnly, Incompatible, Busy, Io, JobRunning };

struct Result {
    Code code = Code::Ok;
    QString message;
    // The offending field for Invalid (a JSON path such as "shape.max.z") or
    // "expectedRevision" for a stale revision.
    QString param;
    std::optional<Region> region;
    std::optional<std::uint64_t> currentRevision;
    bool ok() const { return code == Code::Ok; }
};

struct LoadError {
    std::string file;
    std::string message;
    std::string path;
};

class RegionStore
{
public:
    using Clock = std::function<std::string()>;

    RegionStore();

    // An empty directory means "no saved project": every mutation is refused
    // with NoProject and nothing is written.
    void setDirectory(std::filesystem::path directory);
    const std::filesystem::path& directory() const { return _directory; }
    std::filesystem::path trashDirectory() const;
    std::filesystem::path fileFor(const std::string& id) const;

    // Re-reads every region file.
    void reload();
    const std::vector<Loaded>& regions() const { return _regions; }
    const std::vector<LoadError>& loadErrors() const { return _loadErrors; }
    const Loaded* find(const std::string& id) const;

    // `region` must carry everything but revision and modified, which the
    // store sets (revision 1). The id must be new. A dry run makes every check
    // a save makes, returns the region as it would be written, and writes
    // nothing.
    Result create(Region region, bool dryRun = false);
    // Replaces the region with the same id. Server-owned fields (author,
    // created, frame) are kept from the stored copy; the revision becomes the
    // stored one plus 1. `expectedRevision`, when given, must match.
    Result update(Region next, std::optional<std::uint64_t> expectedRevision, bool dryRun = false);
    // Moves the file to the trash folder.
    Result remove(const std::string& id, std::optional<std::uint64_t> expectedRevision);

    // Hooks for tests.
    void setClock(Clock clock) { _clock = std::move(clock); }
    void setLockTimeoutMs(int timeoutMs) { _lockTimeoutMs = timeoutMs; }

    static QString newId();
    static std::string nowUtc();

private:
    class Lock;

    Result checkWritable(const std::string& id, std::optional<std::uint64_t> expectedRevision, bool requireEditable) const;
    Result moveToTrash(const std::string& id);
    std::optional<std::uint64_t> diskRevision(const std::string& id) const;
    void reloadOne(const std::string& id);
    void insertSorted(Loaded loaded);

    std::filesystem::path _directory;
    std::vector<Loaded> _regions;
    std::vector<LoadError> _loadErrors;
    Clock _clock;
    int _lockTimeoutMs = 2000;
};

// Reads a JSON file; nullopt with `error` set when it cannot be opened or
// parsed.
std::optional<Json> readJsonFile(const std::filesystem::path& path, std::string* error);
// Writes `text` through QSaveFile, so readers see the old file or the new one,
// never a partial write. Returns an empty string or the error.
QString writeFileAtomic(const std::filesystem::path& path, const std::string& text);

QString describeCode(Code code);

}  // namespace vc3d::regions
