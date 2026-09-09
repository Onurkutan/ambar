// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "builder.hpp"

#include <memory>

#include "ambar/iterator.hpp"
#include "file.hpp"
#include "filename.hpp"
#include "table_builder.hpp"

namespace ambar {

Status build_table(const std::string& dbname, const Options& options,
                   TableCache* table_cache, const Comparator* comparator,
                   Iterator* iter, FileMetaData* meta) {
  meta->file_size = 0;
  iter->seek_to_first();
  if (!iter->valid()) return iter->status();

  const std::string path = table_file_name(dbname, meta->number);
  std::unique_ptr<WritableFile> file;
  Status status = WritableFile::open(path, /*append=*/false, &file);
  if (!status.is_ok()) return status;

  {
    TableBuilder builder(options, file.get(), comparator);
    meta->smallest.assign(iter->key().data(), iter->key().size());

    std::string largest;
    for (; iter->valid(); iter->next()) {
      largest.assign(iter->key().data(), iter->key().size());
      builder.add(iter->key(), iter->value());
    }
    meta->largest = largest;

    status = builder.finish();
    if (status.is_ok()) {
      meta->file_size = builder.file_size();
    }
  }

  if (status.is_ok()) status = file->sync();
  if (status.is_ok()) status = file->close();
  file.reset();

  if (status.is_ok()) status = iter->status();

  if (status.is_ok() && meta->file_size > 0) {
    // Opened once here, before anyone depends on it.  A file that will not
    // open is better discovered now -- while it can still be discarded and the
    // memtable kept -- than on the first read after the log has been dropped.
    std::unique_ptr<Iterator> check(
        table_cache->new_iterator(ReadOptions(), meta->number,
                                  meta->file_size));
    status = check->status();
  }

  if (!status.is_ok()) {
    remove_file(path);
  }
  return status;
}

}  // namespace ambar
