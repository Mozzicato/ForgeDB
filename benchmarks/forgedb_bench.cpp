// ForgeDB benchmark harness.
//
// Usage: forgedb_bench <output_dir> [runs]
//
// Configurations:
//   seq_put/nosync         one put = one transaction = one write() call, no fsync
//   seq_put/sync           one put per transaction, fsync per transaction
//   batch_put/B/nosync     B puts per WriteBatch (one write() per batch), no fsync
//   batch_put/B/sync       B puts per WriteBatch, one fsync per batch
//   lookup/index           get() through the hash index over 1M records
//   lookup/scan_full       sequential scan of the whole log (needed to find the
//                          latest version of a key in an append-only log)
//   lookup/scan_early_exit scan that stops at the first match (only correct
//                          because keys in this dataset are never overwritten)
//
// Every (config, run) pair is executed in a shuffled order so background load
// affects all configurations alike. Only one benchmark runs at a time.
// Keys are 16 bytes, values 100 bytes. "Work" for lookups is counted as
// records decoded and bytes examined.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "db.h"

using namespace forgedb;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kValueSize = 100;

std::string make_key(uint64_t i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "key%013llu", static_cast<unsigned long long>(i));  // 16 bytes
    return buf;
}

const std::string& value() {
    static const std::string v(kValueSize, 'v');
    return v;
}

struct Result {
    std::string config;
    int run;
    uint64_t ops;
    double seconds;
    uint64_t records_examined;  // lookups only
    uint64_t bytes_examined;    // lookups only
};

// ---- write benchmarks ---------------------------------------------------------

Result write_bench(const std::string& config, int run, const fs::path& dir, uint64_t ops, int batch, SyncMode sync) {
    const fs::path path = dir / ("w_" + std::to_string(run) + ".fdb");
    fs::remove(path);
    double secs;
    {
        DB db(path, {sync});
        const auto t0 = Clock::now();
        if (batch == 1) {
            for (uint64_t i = 0; i < ops; ++i) db.put(make_key(i), value());
        } else {
            WriteBatch b;
            for (uint64_t i = 0; i < ops; ++i) {
                b.put(make_key(i), value());
                if (b.size() == static_cast<size_t>(batch)) {
                    db.commit(b);
                    b.clear();
                }
            }
            db.commit(b);
        }
        secs = std::chrono::duration<double>(Clock::now() - t0).count();
    }
    fs::remove(path);
    return {config, run, ops, secs, 0, 0};
}

// ---- lookup benchmarks ----------------------------------------------------------

constexpr uint64_t kRecords = 1'000'000;

// Scans the log from the start, decoding every record, looking for `key`.
// Returns the number of records decoded; bytes decoded via `bytes`.
uint64_t scan(std::string_view log, const std::string& key, bool stop_at_first, uint64_t& bytes, bool& found) {
    uint64_t records = 0;
    size_t off = 0;
    Record rec;
    found = false;
    while (off < log.size()) {
        const DecodeResult r = decode_record(log, off, rec);
        if (r.status != DecodeStatus::kOk) break;
        ++records;
        bytes += r.size;
        off += r.size;
        if (rec.type == RecordType::kPut && rec.key == key) {
            found = true;
            if (stop_at_first) break;
        }
    }
    return records;
}

Result lookup_bench(const std::string& config, int run, DB& db, std::string_view log) {
    std::mt19937_64 rng(1000 + run);
    if (config == "lookup/index") {
        constexpr uint64_t kLookups = 100'000;
        std::vector<std::string> keys;
        for (uint64_t i = 0; i < kLookups; ++i) keys.push_back(make_key(rng() % kRecords));
        uint64_t bytes = 0;
        const auto t0 = Clock::now();
        for (const auto& k : keys) bytes += db.get(k)->size();
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        // Each get() reads and decodes exactly one record (16 B key + 100 B value + 25 B header).
        const uint64_t record_bytes = kLookups * (25 + 16 + kValueSize);
        if (bytes != kLookups * kValueSize) std::fprintf(stderr, "index lookup returned wrong sizes\n");
        return {config, run, kLookups, secs, kLookups, record_bytes};
    }
    const bool early = config == "lookup/scan_early_exit";
    constexpr uint64_t kLookups = 20;
    uint64_t records = 0, bytes = 0;
    const auto t0 = Clock::now();
    for (uint64_t i = 0; i < kLookups; ++i) {
        bool found;
        records += scan(log, make_key(rng() % kRecords), early, bytes, found);
        if (!found) std::fprintf(stderr, "scan missed a key\n");
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    return {config, run, kLookups, secs, records, bytes};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output_dir> [runs]\n", argv[0]);
        return 1;
    }
    const fs::path out_dir = argv[1];
    const int runs = argc > 2 ? std::atoi(argv[2]) : 5;
    fs::create_directories(out_dir);
    const fs::path work = fs::temp_directory_path() / "forgedb_bench";
    fs::remove_all(work);
    fs::create_directories(work);

    struct WriteCfg {
        std::string name;
        uint64_t ops;
        int batch;
        SyncMode sync;
    };
    const std::vector<WriteCfg> write_cfgs = {
        {"seq_put/nosync", 200'000, 1, SyncMode::kNone},
        {"batch_put/16/nosync", 200'000, 16, SyncMode::kNone},
        {"batch_put/64/nosync", 200'000, 64, SyncMode::kNone},
        {"batch_put/256/nosync", 200'000, 256, SyncMode::kNone},
        {"seq_put/sync", 2'000, 1, SyncMode::kEveryCommit},
        {"batch_put/16/sync", 32'000, 16, SyncMode::kEveryCommit},
        {"batch_put/64/sync", 64'000, 64, SyncMode::kEveryCommit},
        {"batch_put/256/sync", 200'000, 256, SyncMode::kEveryCommit},
    };
    const std::vector<std::string> lookup_cfgs = {"lookup/index", "lookup/scan_full", "lookup/scan_early_exit"};

    // Build the 1M-record dataset once (not timed).
    std::printf("building %llu-record dataset...\n", static_cast<unsigned long long>(kRecords));
    const fs::path ds_path = work / "dataset.fdb";
    {
        DB db(ds_path, {SyncMode::kNone});
        WriteBatch b;
        for (uint64_t i = 0; i < kRecords; ++i) {
            b.put(make_key(i), value());
            if (b.size() == 1000) {
                db.commit(b);
                b.clear();
            }
        }
        db.commit(b);
    }
    auto dataset = std::make_unique<DB>(ds_path, Options{SyncMode::kNone});
    const std::string log = LogFile(ds_path).read_all();  // scans run over an in-memory copy (best case for scan)

    // Shuffled schedule of every (config, run).
    std::vector<std::pair<std::string, int>> schedule;
    for (int r = 0; r < runs; ++r) {
        for (const auto& c : write_cfgs) schedule.push_back({c.name, r});
        for (const auto& c : lookup_cfgs) schedule.push_back({c, r});
    }
    std::mt19937 shuffle_rng(12345);
    std::shuffle(schedule.begin(), schedule.end(), shuffle_rng);

    std::vector<Result> results;
    for (const auto& [name, r] : schedule) {
        Result res;
        const auto it = std::find_if(write_cfgs.begin(), write_cfgs.end(), [&](const WriteCfg& c) { return c.name == name; });
        if (it != write_cfgs.end()) {
            res = write_bench(name, r, work, it->ops, it->batch, it->sync);
        } else {
            res = lookup_bench(name, r, *dataset, log);
        }
        std::printf("%-24s run %d: %10.0f ops/s\n", name.c_str(), r, res.ops / res.seconds);
        std::fflush(stdout);
        results.push_back(res);
    }
    dataset.reset();  // close the dataset file (Windows cannot delete open files)

    // Raw results.
    std::ofstream raw(out_dir / "raw.csv");
    raw << "config,run,ops,seconds,ops_per_sec,records_examined,bytes_examined\n";
    for (const auto& r : results) {
        raw << r.config << ',' << r.run << ',' << r.ops << ',' << r.seconds << ',' << r.ops / r.seconds << ','
            << r.records_examined << ',' << r.bytes_examined << '\n';
    }

    // Summary: median / min / max ops/sec, and work per lookup.
    std::map<std::string, std::vector<const Result*>> by_cfg;
    for (const auto& r : results) by_cfg[r.config].push_back(&r);
    std::ofstream sum(out_dir / "summary.csv");
    sum << "config,runs,ops_per_run,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,"
           "records_examined_per_op,bytes_examined_per_op\n";
    for (auto& [cfg, rs] : by_cfg) {
        std::vector<double> rates;
        for (const Result* r : rs) rates.push_back(r->ops / r->seconds);
        std::sort(rates.begin(), rates.end());
        const double median = rates.size() % 2 ? rates[rates.size() / 2]
                                               : (rates[rates.size() / 2 - 1] + rates[rates.size() / 2]) / 2;
        uint64_t rec = 0, bytes = 0, ops = 0;
        for (const Result* r : rs) {
            rec += r->records_examined;
            bytes += r->bytes_examined;
            ops += r->ops;
        }
        sum << cfg << ',' << rs.size() << ',' << rs.front()->ops << ',' << median << ',' << rates.front() << ','
            << rates.back() << ',' << static_cast<double>(rec) / ops << ',' << static_cast<double>(bytes) / ops << '\n';
    }
    raw.close();
    sum.close();
    std::error_code ec;
    fs::remove_all(work, ec);
    std::printf("wrote %s and %s\n", (out_dir / "raw.csv").string().c_str(), (out_dir / "summary.csv").string().c_str());
}
