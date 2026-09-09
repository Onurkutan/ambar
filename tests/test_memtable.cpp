#include "harness.hpp"
#include "testutil.hpp"

#include <map>
#include <random>
#include <string>
#include <vector>

#include "../src/memtable.hpp"

using namespace ambar;

namespace {

using ambar_test::MemTableRef;

// Convenience: look up at the newest possible snapshot.
enum class Lookup { kFound, kDeleted, kAbsent };

Lookup lookup(const MemTable& table, std::string_view key, std::string* value,
              SequenceNumber snapshot = kMaxSequenceNumber) {
  Status status;
  if (!table.get(key, snapshot, value, &status)) return Lookup::kAbsent;
  return status.is_ok() ? Lookup::kFound : Lookup::kDeleted;
}

}  // namespace

TEST(MemTable, an_empty_table_finds_nothing) {
  MemTableRef table;
  std::string value;
  CHECK(lookup(*table, "missing", &value) == Lookup::kAbsent);
}

TEST(MemTable, a_written_key_reads_back) {
  MemTableRef table;
  table->add(1, ValueType::kValue, "key", "value");
  std::string value;
  CHECK(lookup(*table, "key", &value) == Lookup::kFound);
  CHECK_EQ(value, std::string("value"));
}

TEST(MemTable, empty_keys_and_values_round_trip) {
  MemTableRef table;
  table->add(1, ValueType::kValue, "", "empty key");
  table->add(2, ValueType::kValue, "empty value", "");
  std::string value;
  CHECK(lookup(*table, "", &value) == Lookup::kFound);
  CHECK_EQ(value, std::string("empty key"));
  CHECK(lookup(*table, "empty value", &value) == Lookup::kFound);
  CHECK_EQ(value.size(), size_t{0});
}

TEST(MemTable, keys_and_values_may_contain_nul_bytes) {
  MemTableRef table;
  const std::string key("k\0k", 3);
  const std::string val("v\0v", 3);
  table->add(1, ValueType::kValue, key, val);
  std::string value;
  CHECK(lookup(*table, key, &value) == Lookup::kFound);
  CHECK_EQ(value, val);
}

TEST(MemTable, the_newest_version_of_a_key_wins) {
  MemTableRef table;
  table->add(1, ValueType::kValue, "k", "old");
  table->add(2, ValueType::kValue, "k", "new");
  std::string value;
  CHECK(lookup(*table, "k", &value) == Lookup::kFound);
  CHECK_EQ(value, std::string("new"));
}

TEST(MemTable, a_tombstone_reports_deleted_rather_than_absent) {
  // The distinction the whole read path depends on: "deleted" must stop the
  // search, "absent" must let it continue to older tables.  Conflating them
  // resurrects deleted keys.
  MemTableRef table;
  table->add(1, ValueType::kValue, "k", "value");
  table->add(2, ValueType::kDeletion, "k", "");
  std::string value;
  CHECK(lookup(*table, "k", &value) == Lookup::kDeleted);
}

TEST(MemTable, a_write_after_a_delete_revives_the_key) {
  MemTableRef table;
  table->add(1, ValueType::kValue, "k", "first");
  table->add(2, ValueType::kDeletion, "k", "");
  table->add(3, ValueType::kValue, "k", "second");
  std::string value;
  CHECK(lookup(*table, "k", &value) == Lookup::kFound);
  CHECK_EQ(value, std::string("second"));
}

TEST(MemTable, a_snapshot_hides_writes_made_after_it) {
  MemTableRef table;
  table->add(10, ValueType::kValue, "k", "at ten");
  table->add(20, ValueType::kValue, "k", "at twenty");

  std::string value;
  CHECK(lookup(*table, "k", &value, 20) == Lookup::kFound);
  CHECK_EQ(value, std::string("at twenty"));
  CHECK(lookup(*table, "k", &value, 19) == Lookup::kFound);
  CHECK_EQ(value, std::string("at ten"));
  CHECK(lookup(*table, "k", &value, 9) == Lookup::kAbsent);
}

TEST(MemTable, a_snapshot_can_see_a_key_before_it_was_deleted) {
  MemTableRef table;
  table->add(10, ValueType::kValue, "k", "alive");
  table->add(20, ValueType::kDeletion, "k", "");
  std::string value;
  CHECK(lookup(*table, "k", &value, 20) == Lookup::kDeleted);
  CHECK(lookup(*table, "k", &value, 19) == Lookup::kFound);
  CHECK_EQ(value, std::string("alive"));
}

TEST(MemTable, a_lookup_never_strays_onto_a_neighbouring_key) {
  // The seek lands on the first entry >= the lookup key, which for a missing
  // key is some *other* key.  Failing to check the user key would return that
  // neighbour's value.
  MemTableRef table;
  table->add(1, ValueType::kValue, "aaa", "first");
  table->add(2, ValueType::kValue, "ccc", "third");
  std::string value;
  CHECK(lookup(*table, "bbb", &value) == Lookup::kAbsent);
  CHECK(lookup(*table, "zzz", &value) == Lookup::kAbsent);
}

TEST(MemTable, iteration_yields_internal_keys_in_order) {
  MemTableRef table;
  table->add(1, ValueType::kValue, "b", "1");
  table->add(2, ValueType::kValue, "a", "2");
  table->add(3, ValueType::kValue, "b", "3");  // newer version of b

  std::vector<std::pair<std::string, SequenceNumber>> seen;
  MemTable::Iterator iter(table.get());
  for (iter.seek_to_first(); iter.valid(); iter.next()) {
    seen.emplace_back(std::string(extract_user_key(iter.key())),
                      extract_sequence(iter.key()));
  }
  // a first, then b's versions newest-first.
  const std::vector<std::pair<std::string, SequenceNumber>> want = {
      {"a", 2}, {"b", 3}, {"b", 1}};
  CHECK_EQ(seen.size(), want.size());
  for (size_t i = 0; i < seen.size() && i < want.size(); ++i) {
    CHECK_EQ(seen[i].first, want[i].first);
    CHECK_EQ(seen[i].second, want[i].second);
  }
}

TEST(MemTable, iterator_values_match_what_was_written) {
  MemTableRef table;
  table->add(1, ValueType::kValue, "k1", "v1");
  table->add(2, ValueType::kValue, "k2", "");
  table->add(3, ValueType::kDeletion, "k3", "");

  MemTable::Iterator iter(table.get());
  iter.seek_to_first();
  CHECK(iter.valid());
  CHECK_EQ(std::string(iter.value()), std::string("v1"));
  iter.next();
  CHECK(iter.valid());
  CHECK_EQ(iter.value().size(), size_t{0});
  iter.next();
  CHECK(iter.valid());
  CHECK(extract_value_type(iter.key()) == ValueType::kDeletion);
}

TEST(MemTable, memory_usage_grows_with_the_data) {
  MemTableRef table;
  const size_t empty = table->approximate_memory_usage();
  for (int i = 0; i < 1000; ++i) {
    table->add(static_cast<SequenceNumber>(i + 1), ValueType::kValue,
               "key" + std::to_string(i), std::string(200, 'x'));
  }
  CHECK(table->approximate_memory_usage() > empty + 200 * 1000);
}

TEST(MemTable, agrees_with_a_std_map_over_random_operations) {
  // The blunt check: drive both through the same sequence of writes and
  // deletes, then compare on every key in the space.
  MemTableRef table;
  std::map<std::string, std::string> model;
  std::map<std::string, bool> deleted;

  std::mt19937 rng(2024);
  SequenceNumber sequence = 0;
  for (int i = 0; i < 8000; ++i) {
    const std::string key = "k" + std::to_string(rng() % 400);
    ++sequence;
    if (rng() % 4 == 0) {
      table->add(sequence, ValueType::kDeletion, key, "");
      model.erase(key);
      deleted[key] = true;
    } else {
      const std::string value = "v" + std::to_string(rng());
      table->add(sequence, ValueType::kValue, key, value);
      model[key] = value;
      deleted[key] = false;
    }
  }

  for (int i = 0; i < 400; ++i) {
    const std::string key = "k" + std::to_string(i);
    std::string value;
    const Lookup result = lookup(*table, key, &value);
    const auto it = model.find(key);
    if (it != model.end()) {
      CHECK(result == Lookup::kFound);
      CHECK_EQ(value, it->second);
    } else if (deleted.count(key) != 0) {
      CHECK(result == Lookup::kDeleted);
    } else {
      CHECK(result == Lookup::kAbsent);
    }
  }
}
