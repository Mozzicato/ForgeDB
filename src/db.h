#pragma once

// ForgeDB: an embedded key-value store.
//
// Design (Bitcask-style):
//  - One append-only log file is both the data file and the write-ahead log.
//  - An in-memory hash index maps every live key to the location of its
//    latest record in the log. It is not persisted; it is rebuilt by replaying
//    the log on open.
//  - Every write is a transaction: its PUT/DELETE records followed by a COMMIT
//    record, appended in a single write. A transaction is visible after a
//    restart only if its COMMIT record is intact.
//
// Thread safety: all public methods may be called concurrently.
//  - write_mutex_ (std::mutex) serialises writers: one transaction at a time
//    is encoded, appended, fsynced and published. It is held across the slow
//    disk work.
//  - index_mutex_ (std::shared_mutex) protects the index. get() takes it
//    shared; a writer takes it exclusive only for the short in-memory index
//    update after its data is durable. So readers never wait for an fsync.
//  - get() reads the record from disk AFTER releasing the lock. That is safe
//    because the log is append-only: bytes at an offset never change once
//    written, so a location copied out of the index stays valid.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "log_file.h"
#include "record.h"
#include "replay.h"

namespace forgedb {

enum class SyncMode {
    kEveryCommit,  // fsync before commit() returns: an acknowledged write survives power loss
    kNone,         // leave flushing to the OS: survives a process crash, not power loss
};

struct Options {
    SyncMode sync = SyncMode::kEveryCommit;
};

// Thrown when a record read from disk fails its checksum.
class CorruptionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A set of writes that commit atomically: after a crash either all of them
// are visible or none are. Ops are applied in the order they were added.
class WriteBatch {
public:
    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);
    size_t size() const { return ops_.size(); }
    bool empty() const { return ops_.empty(); }
    void clear() { ops_.clear(); }

private:
    friend class DB;
    struct Op {
        RecordType type;
        std::string key;
        std::string value;
    };
    std::vector<Op> ops_;
};

class DB {
public:
    // Opens (or creates) the database file and runs recovery.
    explicit DB(const std::filesystem::path& path, Options options = {});

    void put(std::string_view key, std::string_view value);
    std::optional<std::string> get(std::string_view key) const;
    // Returns true if the key existed. Removing a missing key writes nothing.
    bool remove(std::string_view key);
    // Commits all ops in the batch atomically. An empty batch is a no-op.
    void commit(const WriteBatch& batch);

    size_t key_count() const;
    uint64_t log_size() const;
    // What recovery found when this DB was opened.
    const ReplayReport& recovery_report() const { return recovery_; }

private:
    // Where the latest record of a key lives in the log.
    struct Location {
        uint64_t offset;
        uint32_t size;
    };
    // Lets the index be searched with a string_view without allocating a
    // temporary std::string (C++20 heterogeneous lookup).
    struct KeyHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    using Index = std::unordered_map<std::string, Location, KeyHash, std::equal_to<>>;

    void apply_to_index(uint64_t base_offset, const std::vector<LoggedOp>& ops);
    // Does the work of commit(). Caller must hold write_mutex_.
    void commit_locked(const WriteBatch& batch);

    const Options options_;
    LogFile file_;
    ReplayReport recovery_;     // written only in the constructor

    mutable std::shared_mutex index_mutex_;
    Index index_;               // guarded by index_mutex_

    mutable std::mutex write_mutex_;
    uint64_t end_offset_ = 0;   // guarded by write_mutex_: where the next transaction goes
    uint64_t next_txn_id_ = 1;  // guarded by write_mutex_
    bool failed_ = false;       // guarded by write_mutex_: a write/fsync failed, refuse writes
};

}  // namespace forgedb
