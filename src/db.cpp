#include "db.h"

#include "fault.h"

namespace forgedb {

void WriteBatch::put(std::string_view key, std::string_view value) {
    ops_.push_back({RecordType::kPut, std::string(key), std::string(value)});
}

void WriteBatch::remove(std::string_view key) { ops_.push_back({RecordType::kDelete, std::string(key), ""}); }

// The constructor runs before any other thread can see this object, so it
// does not need to lock.
DB::DB(const std::filesystem::path& path, Options options) : options_(options), file_(path) {
    // Recovery: replay the whole log and rebuild the index from committed
    // transactions only.
    const std::string log = file_.read_all();
    recovery_ = replay_log(log, [this](uint64_t, const std::vector<LoggedOp>& ops) { apply_to_index(0, ops); });

    // Cut off everything after the last good COMMIT (a torn tail, an
    // uncommitted transaction, or garbage after a corrupt record). Otherwise
    // new transactions would be appended behind bytes that replay can never
    // get past, and they would be lost on the next restart.
    if (recovery_.valid_bytes < log.size()) {
        file_.truncate(recovery_.valid_bytes);
        file_.sync();
    }
    end_offset_ = recovery_.valid_bytes;
    next_txn_id_ = recovery_.last_txn_id + 1;
}

void DB::apply_to_index(uint64_t base_offset, const std::vector<LoggedOp>& ops) {
    for (const LoggedOp& op : ops) {
        if (op.type == RecordType::kPut) {
            index_.insert_or_assign(op.key, Location{base_offset + op.record_offset, op.record_size});
        } else {
            index_.erase(op.key);
        }
    }
}

void DB::put(std::string_view key, std::string_view value) {
    WriteBatch batch;
    batch.put(key, value);
    commit(batch);
}

bool DB::remove(std::string_view key) {
    // Check-then-act must be atomic with respect to other writers, so hold the
    // writer lock across both. Otherwise two concurrent removes of the same
    // key could both see it and both return true.
    std::lock_guard<std::mutex> writer(write_mutex_);
    {
        std::shared_lock<std::shared_mutex> read(index_mutex_);
        if (index_.find(key) == index_.end()) return false;
    }
    WriteBatch batch;
    batch.remove(key);
    commit_locked(batch);
    return true;
}

std::optional<std::string> DB::get(std::string_view key) const {
    Location loc;
    {
        std::shared_lock<std::shared_mutex> read(index_mutex_);
        const auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        loc = it->second;
    }
    // Disk read happens without the lock (see thread-safety note in db.h).
    // The whole record is read, not just the value, so the CRC is checked on
    // every read and on-disk damage is reported instead of returned.
    const std::string bytes = file_.read_at(loc.offset, loc.size);
    Record rec;
    if (decode_record(bytes, 0, rec).status != DecodeStatus::kOk || rec.key != key) {
        throw CorruptionError("record for key failed checksum at offset " + std::to_string(loc.offset));
    }
    return std::move(rec.value);
}

void DB::commit(const WriteBatch& batch) {
    std::lock_guard<std::mutex> writer(write_mutex_);
    commit_locked(batch);
}

void DB::commit_locked(const WriteBatch& batch) {
    if (batch.empty()) return;
    if (failed_) throw IoError("database is read-only after an earlier write/sync failure");

    // 1. Encode every op plus the COMMIT record into one buffer.
    const uint64_t txn_id = next_txn_id_;
    std::string buf;
    std::vector<LoggedOp> ops;
    ops.reserve(batch.ops_.size());
    for (const WriteBatch::Op& op : batch.ops_) {
        const uint64_t at = buf.size();
        const size_t size = encode_record({txn_id, op.type, op.key, op.value}, buf);
        ops.push_back({op.type, op.key, at, static_cast<uint32_t>(size)});
    }
    encode_record({txn_id, RecordType::kCommit, "", encode_commit_count(static_cast<uint32_t>(ops.size()))}, buf);

    // 2. Append with one write, then fsync. This is the durability point.
    //    Readers are not blocked here: only write_mutex_ is held.
    fault::trigger(fault::Point::kBeforeWrite);
    try {
        file_.write_at(end_offset_, buf);
        fault::trigger(fault::Point::kAfterWriteBeforeSync);
        if (options_.sync == SyncMode::kEveryCommit) file_.sync();
    } catch (...) {
        // After a failed fsync the OS may already have dropped the dirty pages,
        // so retrying could falsely report success. Stop accepting writes.
        failed_ = true;
        throw;
    }

    fault::trigger(fault::Point::kAfterSyncBeforePublish);

    // 3. Publish: make the whole batch visible to readers in one step.
    {
        std::unique_lock<std::shared_mutex> exclusive(index_mutex_);
        apply_to_index(end_offset_, ops);
    }
    end_offset_ += buf.size();
    next_txn_id_++;
}

size_t DB::key_count() const {
    std::shared_lock<std::shared_mutex> read(index_mutex_);
    return index_.size();
}

uint64_t DB::log_size() const {
    std::lock_guard<std::mutex> writer(write_mutex_);
    return end_offset_;
}

}  // namespace forgedb
