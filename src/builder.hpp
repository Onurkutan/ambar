// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
#ifndef AMBAR_BUILDER_HPP_
#define AMBAR_BUILDER_HPP_

#include <string>

#include "ambar/options.hpp"
#include "ambar/status.hpp"
#include "comparator.hpp"
#include "table_cache.hpp"
#include "version_edit.hpp"

namespace ambar {

class Iterator;

// Writes everything `iter` yields into a new table file, and fills in `meta`.
//
// The file is fsynced before this returns.  It has to be: the caller is about
// to record the file in the manifest and drop the log that held the same data,
// so a file that is only in the page cache would take the data with it.
//
// An empty input produces no file at all, and meta->file_size is left zero to
// say so.  Writing an empty table would be harmless but would leave the
// version set referencing a file with no keys, which every later compaction
// would then carry around.
Status build_table(const std::string& dbname, const Options& options,
                   TableCache* table_cache, const Comparator* comparator,
                   Iterator* iter, FileMetaData* meta);

}  // namespace ambar

#endif  // AMBAR_BUILDER_HPP_
