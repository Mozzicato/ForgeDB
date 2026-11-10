#include "log_file.h"

#include <gtest/gtest.h>

#include "test_util.h"

using namespace forgedb;
using forgedb::test::TempDir;

TEST(LogFile, NewFileIsEmpty) {
    TempDir dir;
    LogFile f(dir.file("a.log"));
    EXPECT_EQ(f.size(), 0u);
    EXPECT_EQ(f.read_all(), "");
}

TEST(LogFile, WriteThenReadAtOffsets) {
    TempDir dir;
    LogFile f(dir.file("a.log"));
    f.write_at(0, "hello");
    f.write_at(5, "world");
    EXPECT_EQ(f.size(), 10u);
    EXPECT_EQ(f.read_at(3, 4), "lowo");
    EXPECT_EQ(f.read_all(), "helloworld");
}

TEST(LogFile, DataSurvivesReopen) {
    TempDir dir;
    {
        LogFile f(dir.file("a.log"));
        f.write_at(0, "durable");
        f.sync();
    }
    LogFile f(dir.file("a.log"));
    EXPECT_EQ(f.read_all(), "durable");
}

TEST(LogFile, TruncateDropsTail) {
    TempDir dir;
    LogFile f(dir.file("a.log"));
    f.write_at(0, "0123456789");
    f.truncate(4);
    EXPECT_EQ(f.size(), 4u);
    EXPECT_EQ(f.read_all(), "0123");
}

TEST(LogFile, ReadPastEndThrows) {
    TempDir dir;
    LogFile f(dir.file("a.log"));
    f.write_at(0, "abc");
    EXPECT_THROW(f.read_at(1, 10), IoError);
}

TEST(LogFile, OpenInMissingDirectoryThrows) {
    TempDir dir;
    EXPECT_THROW(LogFile(dir.path() / "no_such_dir" / "a.log"), IoError);
}
