#include "RegionStore.hpp"

#include "VCSettings.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace vc3d::regions
{

namespace
{

constexpr int kStaleLockMs = 30000;

QString toQString(const fs::path& path)
{
    return vc3d::pathToQString(path);
}

QString busyMessage()
{
    return QStringLiteral("The regions folder is busy (locked by another process). Try again.");
}

QString noProjectMessage()
{
    return QStringLiteral("Save the project first; regions are stored next to the project file.");
}

QString conflictMessage()
{
    return QStringLiteral("Region changed on disk (another VC3D?). Reloaded it; your change was not saved.");
}

Result make(Code code, QString message, QString param = {})
{
    Result result;
    result.code = code;
    result.message = std::move(message);
    result.param = std::move(param);
    return result;
}

bool endsWith(const std::string& text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::optional<Json> readJsonFile(const fs::path& path, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "could not open the file";
        }
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    try {
        return Json::parse(buffer.str());
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("invalid JSON: ") + e.what();
        }
        return std::nullopt;
    }
}

QString writeFileAtomic(const fs::path& path, const std::string& text)
{
    QSaveFile file(toQString(path));
    if (!file.open(QIODevice::WriteOnly)) {
        return QStringLiteral("Could not write %1: %2").arg(toQString(path), file.errorString());
    }
    if (file.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size())) {
        const QString reason = file.errorString();
        file.cancelWriting();
        return QStringLiteral("Could not write %1: %2").arg(toQString(path), reason);
    }
    if (!file.commit()) {
        return QStringLiteral("Could not write %1: %2").arg(toQString(path), file.errorString());
    }
    return {};
}

class RegionStore::Lock
{
public:
    Lock(const fs::path& directory, int timeoutMs) : _file(toQString(directory / ".regions.lock"))
    {
        _file.setStaleLockTime(kStaleLockMs);
        _locked = _file.tryLock(timeoutMs);
    }
    bool locked() const { return _locked; }

private:
    QLockFile _file;
    bool _locked = false;
};

RegionStore::RegionStore() : _clock(&RegionStore::nowUtc) {}

QString RegionStore::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

std::string RegionStore::nowUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
}

void RegionStore::setDirectory(fs::path directory)
{
    _directory = std::move(directory);
    reload();
}

fs::path RegionStore::trashDirectory() const
{
    return _directory.empty() ? fs::path{} : _directory / ".trash";
}

fs::path RegionStore::fileFor(const std::string& id) const
{
    return _directory / (id + std::string(kFileSuffix));
}

void RegionStore::reload()
{
    _regions.clear();
    _loadErrors.clear();
    if (_directory.empty()) {
        return;
    }
    std::error_code ec;
    if (!fs::is_directory(_directory, ec)) {
        return;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(_directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == '.' || !endsWith(name, kFileSuffix)) {
            continue;
        }
        std::error_code typeError;
        if (!entry.is_regular_file(typeError)) {
            continue;
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        const std::string name = path.filename().string();
        std::string error;
        const auto document = readJsonFile(path, &error);
        if (!document) {
            _loadErrors.push_back({name, error, {}});
            continue;
        }
        ParseResult parsed = parseRegion(*document);
        if (!parsed.loaded) {
            _loadErrors.push_back({name, parsed.error, parsed.errorPath});
            continue;
        }
        const std::string stem = name.substr(0, name.size() - kFileSuffix.size());
        if (parsed.loaded->region.id != stem) {
            _loadErrors.push_back({name, "the id \"" + parsed.loaded->region.id + "\" does not match the file name", "id"});
            continue;
        }
        insertSorted(std::move(*parsed.loaded));
    }
}

const Loaded* RegionStore::find(const std::string& id) const
{
    const auto it = std::find_if(_regions.begin(), _regions.end(), [&id](const Loaded& l) { return l.region.id == id; });
    return it == _regions.end() ? nullptr : &*it;
}

void RegionStore::insertSorted(Loaded loaded)
{
    _regions
        .erase(std::remove_if(_regions.begin(), _regions.end(), [&](const Loaded& l) { return l.region.id == loaded.region.id; }), _regions.end());
    const auto before = [](const Loaded& a, const Loaded& b) {
        if (a.region.created != b.region.created) {
            return a.region.created < b.region.created;
        }
        return a.region.id < b.region.id;
    };
    const auto it = std::upper_bound(_regions.begin(), _regions.end(), loaded, before);
    _regions.insert(it, std::move(loaded));
}

std::optional<std::uint64_t> RegionStore::diskRevision(const std::string& id) const
{
    const auto document = readJsonFile(fileFor(id), nullptr);
    if (!document || !document->is_object() || !document->contains("revision") || !(*document)["revision"].is_number_integer()) {
        return std::nullopt;
    }
    const auto& value = (*document)["revision"];
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    const long long signedValue = value.get<long long>();
    if (signedValue < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(signedValue);
}

void RegionStore::reloadOne(const std::string& id)
{
    _regions.erase(std::remove_if(_regions.begin(), _regions.end(), [&](const Loaded& l) { return l.region.id == id; }), _regions.end());
    const auto document = readJsonFile(fileFor(id), nullptr);
    if (!document) {
        return;
    }
    ParseResult parsed = parseRegion(*document);
    if (parsed.loaded && parsed.loaded->region.id == id) {
        insertSorted(std::move(*parsed.loaded));
    }
}

Result RegionStore::moveToTrash(const std::string& id)
{
    const fs::path source = fileFor(id);
    const fs::path trash = trashDirectory();
    std::error_code ec;
    fs::create_directories(trash, ec);
    if (ec) {
        return make(Code::Io, QStringLiteral("Could not write %1: %2").arg(toQString(trash), QString::fromStdString(ec.message())));
    }
    const fs::path target = trash / source.filename();
    fs::remove(target, ec);
    ec.clear();
    fs::rename(source, target, ec);
    if (ec) {
        return make(Code::Io, QStringLiteral("Could not move %1 to %2: %3").arg(toQString(source), toQString(target), QString::fromStdString(ec.message())));
    }
    _regions.erase(std::remove_if(_regions.begin(), _regions.end(), [&](const Loaded& l) { return l.region.id == id; }), _regions.end());
    return {};
}

Result RegionStore::checkWritable(const std::string& id, std::optional<std::uint64_t> expectedRevision, bool requireEditable) const
{
    if (_directory.empty()) {
        return make(Code::NoProject, noProjectMessage());
    }
    const Loaded* existing = find(id);
    if (!existing) {
        return make(Code::NotFound, QStringLiteral("No region with id %1").arg(QString::fromStdString(id)), QStringLiteral("id"));
    }
    if (requireEditable && existing->readOnly) {
        return make(
            Code::ReadOnly,
            QStringLiteral("Region %1 is read-only: %2").arg(QString::fromStdString(existing->region.name), QString::fromStdString(existing->readOnlyReason)));
    }
    if (expectedRevision && *expectedRevision != existing->region.revision) {
        Result result =
            make(Code::Conflict, QStringLiteral("Stale revision: the region is at revision %1, not %2").arg(existing->region.revision).arg(*expectedRevision), QStringLiteral("expectedRevision"));
        result.currentRevision = existing->region.revision;
        return result;
    }
    return {};
}

Result RegionStore::create(Region region)
{
    if (_directory.empty()) {
        return make(Code::NoProject, noProjectMessage());
    }
    if (region.created.empty()) {
        region.created = _clock();
    }
    region.modified = region.created;
    region.revision = 1;
    if (const auto problem = validateRegion(region)) {
        return make(Code::Invalid, QString::fromStdString(problem->first + ": " + problem->second), QString::fromStdString(problem->first));
    }
    if (find(region.id)) {
        return make(Code::Conflict, QStringLiteral("A region with id %1 already exists").arg(QString::fromStdString(region.id)), QStringLiteral("id"));
    }
    std::error_code ec;
    fs::create_directories(_directory, ec);
    if (ec) {
        return make(Code::Io, QStringLiteral("Could not write %1: %2").arg(toQString(_directory), QString::fromStdString(ec.message())));
    }
    Lock lock(_directory, _lockTimeoutMs);
    if (!lock.locked()) {
        return make(Code::Busy, busyMessage());
    }
    if (fs::exists(fileFor(region.id), ec)) {
        reloadOne(region.id);
        return make(Code::Conflict, conflictMessage(), QStringLiteral("id"));
    }
    if (const QString error = writeFileAtomic(fileFor(region.id), toJson(region).dump(2) + "\n"); !error.isEmpty()) {
        return make(Code::Io, error);
    }
    insertSorted(Loaded{region, false, {}});
    Result result;
    result.region = std::move(region);
    return result;
}

Result RegionStore::update(Region next, std::optional<std::uint64_t> expectedRevision)
{
    if (Result check = checkWritable(next.id, expectedRevision, true); !check.ok()) {
        return check;
    }
    const Region& stored = find(next.id)->region;
    const std::uint64_t revision = stored.revision;
    next.author = stored.author;
    next.created = stored.created;
    next.frame = stored.frame;
    next.revision = revision + 1;
    next.modified = _clock();
    if (const auto problem = validateRegion(next)) {
        return make(Code::Invalid, QString::fromStdString(problem->first + ": " + problem->second), QString::fromStdString(problem->first));
    }
    Lock lock(_directory, _lockTimeoutMs);
    if (!lock.locked()) {
        return make(Code::Busy, busyMessage());
    }
    const auto onDisk = diskRevision(next.id);
    if (!onDisk || *onDisk != revision) {
        reloadOne(next.id);
        Result result = make(Code::Conflict, conflictMessage(), QStringLiteral("expectedRevision"));
        if (const Loaded* reloaded = find(next.id)) {
            result.currentRevision = reloaded->region.revision;
        }
        return result;
    }
    if (const QString error = writeFileAtomic(fileFor(next.id), toJson(next).dump(2) + "\n"); !error.isEmpty()) {
        return make(Code::Io, error);
    }
    insertSorted(Loaded{next, false, {}});
    Result result;
    result.region = std::move(next);
    return result;
}

Result RegionStore::remove(const std::string& id, std::optional<std::uint64_t> expectedRevision)
{
    if (Result check = checkWritable(id, expectedRevision, false); !check.ok()) {
        return check;
    }
    const Region before = find(id)->region;
    Lock lock(_directory, _lockTimeoutMs);
    if (!lock.locked()) {
        return make(Code::Busy, busyMessage());
    }
    const auto onDisk = diskRevision(id);
    if (!onDisk || *onDisk != before.revision) {
        reloadOne(id);
        Result result = make(Code::Conflict, conflictMessage(), QStringLiteral("expectedRevision"));
        if (const Loaded* reloaded = find(id)) {
            result.currentRevision = reloaded->region.revision;
        }
        return result;
    }
    if (Result moved = moveToTrash(id); !moved.ok()) {
        return moved;
    }
    Result result;
    result.region = before;
    return result;
}

QString describeCode(Code code)
{
    switch (code) {
        case Code::Ok:
            return QStringLiteral("ok");
        case Code::NoProject:
            return QStringLiteral("no project");
        case Code::NoVolume:
            return QStringLiteral("no volume");
        case Code::Invalid:
            return QStringLiteral("invalid");
        case Code::NotFound:
            return QStringLiteral("not found");
        case Code::Conflict:
            return QStringLiteral("conflict");
        case Code::ReadOnly:
            return QStringLiteral("read-only");
        case Code::Incompatible:
            return QStringLiteral("incompatible");
        case Code::Busy:
            return QStringLiteral("busy");
        case Code::Io:
            return QStringLiteral("write failure");
        case Code::JobRunning:
            return QStringLiteral("job running");
    }
    return {};
}

}  // namespace vc3d::regions
