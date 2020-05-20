#include "raft/snapshot.h"
#include "random/generators.h"
#include "seastarx.h"

#include <seastar/testing/thread_test_case.hh>

SEASTAR_THREAD_TEST_CASE(missing_snapshot_is_not_error) {
    raft::snapshot_manager mgr("d/n/e", ss::default_priority_class());
    auto reader = mgr.open_snapshot().get0();
    BOOST_REQUIRE(!reader);
}

SEASTAR_THREAD_TEST_CASE(reading_from_empty_snapshot_is_error) {
    raft::snapshot_manager mgr(".", ss::default_priority_class());
    try {
        ss::remove_file(mgr.snapshot_path().string()).get();
    } catch (...) {
    }

    auto fd = ss::open_file_dma(
                mgr.snapshot_path().string(),
                ss::open_flags::wo | ss::open_flags::create)
                .get0();
    fd.truncate(0).get();
    fd.close().get();

    auto reader = mgr.open_snapshot().get0();
    BOOST_REQUIRE(reader);
    BOOST_CHECK_EXCEPTION(
      reader->read_metadata().get0(),
      std::runtime_error,
      [](std::runtime_error e) {
          return std::string(e.what()).find(
                   "Snapshot file does not contain full header")
                 != std::string::npos;
      });
}

SEASTAR_THREAD_TEST_CASE(reader_verifies_header_crc) {
    raft::snapshot_manager mgr(".", ss::default_priority_class());
    try {
        ss::remove_file(mgr.snapshot_path().string()).get();
    } catch (...) {
    }

    auto writer = mgr.start_snapshot().get0();
    writer.write_metadata(raft::snapshot_metadata()).get0();
    writer.close().get();
    mgr.finish_snapshot(writer).get();

    {
        // write some junk into the metadata. we're not using seastar i/o here
        // because for a test its too much to deal with i/o alignment, etc..
        int fd = ::open(mgr.snapshot_path().c_str(), O_WRONLY);
        BOOST_REQUIRE(fd > 0);
        ::write(fd, &fd, sizeof(fd));
        ::fsync(fd);
        ::close(fd);
    }

    auto reader = mgr.open_snapshot().get0();
    BOOST_REQUIRE(reader);
    BOOST_CHECK_EXCEPTION(
      reader->read_metadata().get0(),
      std::runtime_error,
      [](std::runtime_error e) {
          return std::string(e.what()).find("Failed to verify header crc")
                 != std::string::npos;
      });
}

SEASTAR_THREAD_TEST_CASE(reader_verifies_metadata_crc) {
    raft::snapshot_manager mgr(".", ss::default_priority_class());
    try {
        ss::remove_file(mgr.snapshot_path().string()).get();
    } catch (...) {
    }

    auto writer = mgr.start_snapshot().get0();
    writer.write_metadata(raft::snapshot_metadata()).get0();
    writer.close().get();
    mgr.finish_snapshot(writer).get();

    {
        // write some junk into the header. we're not using seastar i/o here
        // because for a test its too much to deal with i/o alignment, etc..
        int fd = ::open(mgr.snapshot_path().c_str(), O_WRONLY);
        BOOST_REQUIRE(fd > 0);
        ::lseek(fd, raft::snapshot_header::ondisk_size, SEEK_SET);
        ::write(fd, &fd, sizeof(fd));
        ::fsync(fd);
        ::close(fd);
    }

    auto reader = mgr.open_snapshot().get0();
    BOOST_REQUIRE(reader);
    BOOST_CHECK_EXCEPTION(
      reader->read_metadata().get0(),
      std::runtime_error,
      [](std::runtime_error e) {
          return std::string(e.what()).find("Failed to verify metadata crc")
                 != std::string::npos;
      });
}

SEASTAR_THREAD_TEST_CASE(read_write) {
    raft::snapshot_manager mgr(".", ss::default_priority_class());
    try {
        ss::remove_file(mgr.snapshot_path().string()).get();
    } catch (...) {
    }

    raft::snapshot_metadata metadata{
      .last_included_index = model::offset(9),
      .last_included_term = model::term_id(33),
    };

    const auto blob = random_generators::gen_alphanum_string(1234);

    auto writer = mgr.start_snapshot().get0();
    writer.write_metadata(metadata).get();
    writer.output().write(blob).get();
    writer.close().get();
    mgr.finish_snapshot(writer).get();

    auto reader = mgr.open_snapshot().get0();
    BOOST_REQUIRE(reader);
    auto read_metadata = reader->read_metadata().get0();
    BOOST_TEST(
      read_metadata.last_included_index == metadata.last_included_index);
    BOOST_TEST(read_metadata.last_included_term == metadata.last_included_term);
    auto blob_read = reader->input().read_exactly(blob.size()).get0();
    BOOST_TEST(blob_read.size() == 1234);
    BOOST_TEST(blob == ss::to_sstring(std::move(blob_read)));
}

SEASTAR_THREAD_TEST_CASE(remove_partial_snapshots) {
    raft::snapshot_manager mgr(".", ss::default_priority_class());

    auto mk_partial = [&] {
        auto writer = mgr.start_snapshot().get0();
        writer.close().get();
        return writer.path();
    };

    auto p1 = mk_partial();
    auto p2 = mk_partial();

    BOOST_REQUIRE(ss::file_exists(p1.string()).get0());
    BOOST_REQUIRE(ss::file_exists(p2.string()).get0());

    mgr.remove_partial_snapshots().get();

    BOOST_REQUIRE(!ss::file_exists(p1.string()).get0());
    BOOST_REQUIRE(!ss::file_exists(p2.string()).get0());
}