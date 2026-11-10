#include "replay.h"

namespace forgedb {

ReplayReport replay_log(std::string_view log, const ApplyFn& apply) {
    ReplayReport report;
    report.total_bytes = log.size();

    std::vector<LoggedOp> pending;  // ops of the transaction being read
    uint64_t pending_txn = 0;
    size_t offset = 0;
    Record rec;

    while (true) {
        if (offset == log.size()) {
            report.stop = ReplayStop::kCleanEnd;
            break;
        }
        const DecodeResult r = decode_record(log, offset, rec);
        if (r.status == DecodeStatus::kTruncated) {
            report.stop = ReplayStop::kTornTail;
            break;
        }
        if (r.status == DecodeStatus::kCorrupt) {
            report.stop = ReplayStop::kCorruptRecord;
            break;
        }
        report.records_read++;

        if (rec.type == RecordType::kCommit) {
            uint32_t count = 0;
            const bool ok = !pending.empty() && rec.txn_id == pending_txn &&
                            decode_commit_count(rec.value, count) && count == pending.size();
            if (!ok) {
                report.stop = ReplayStop::kBadTransaction;
                break;
            }
            apply(pending_txn, pending);
            report.txns_applied++;
            report.last_txn_id = pending_txn;
            report.valid_bytes = offset + r.size;
            pending.clear();
        } else {
            if (pending.empty()) {
                // First record of a new transaction: ids must keep increasing.
                if (rec.txn_id <= report.last_txn_id) {
                    report.stop = ReplayStop::kBadTransaction;
                    break;
                }
                pending_txn = rec.txn_id;
            } else if (rec.txn_id != pending_txn) {
                // A new transaction started before the previous one committed.
                report.stop = ReplayStop::kBadTransaction;
                break;
            }
            pending.push_back({rec.type, std::move(rec.key), offset, static_cast<uint32_t>(r.size)});
        }
        offset += r.size;
    }

    if (!pending.empty()) report.txns_discarded = 1;
    return report;
}

const char* to_string(ReplayStop stop) {
    switch (stop) {
        case ReplayStop::kCleanEnd: return "clean_end";
        case ReplayStop::kTornTail: return "torn_tail";
        case ReplayStop::kCorruptRecord: return "corrupt_record";
        case ReplayStop::kBadTransaction: return "bad_transaction";
    }
    return "unknown";
}

}  // namespace forgedb
