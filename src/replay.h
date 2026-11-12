#pragma once

// WAL replay: turns the raw bytes of a log into the list of committed
// transactions, in log order.
//
// Rules (this is the recovery protocol):
//  1. Records are read front to back.
//  2. PUT/DELETE records are buffered until the COMMIT record of their
//     transaction arrives; only then is the transaction handed to `apply`.
//  3. Replay stops at the FIRST record that is torn, fails its CRC, or breaks
//     the transaction rules. Nothing after that point is trusted.
//  4. A transaction without a COMMIT (e.g. the process died mid-write) is
//     discarded.
//
// The result is "point-in-time" recovery: the state equals the state right
// after the last intact COMMIT record. `valid_bytes` is where that commit
// ends; the engine truncates the file there so new writes never land behind
// garbage.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "record.h"

namespace forgedb {

struct LoggedOp {
    RecordType type;           // kPut or kDelete
    std::string key;
    uint64_t record_offset;    // where the record starts in the log
    uint32_t record_size;      // encoded size of the record
};

enum class ReplayStop {
    kCleanEnd,          // reached end of log exactly on a record boundary
    kTornTail,          // last record is incomplete
    kCorruptRecord,     // CRC mismatch or malformed record
    kBadTransaction,    // records intact but violate txn rules (wrong id, count)
};

struct ReplayReport {
    ReplayStop stop = ReplayStop::kCleanEnd;
    uint64_t records_read = 0;       // intact records decoded
    uint64_t txns_applied = 0;
    uint64_t txns_discarded = 0;     // started but never committed
    uint64_t valid_bytes = 0;        // end offset of the last applied COMMIT
    uint64_t total_bytes = 0;        // size of the log we were given
    uint64_t last_txn_id = 0;        // id of the last applied transaction
};

using ApplyFn = std::function<void(uint64_t txn_id, const std::vector<LoggedOp>& ops)>;

ReplayReport replay_log(std::string_view log, const ApplyFn& apply);

const char* to_string(ReplayStop stop);

}  // namespace forgedb
