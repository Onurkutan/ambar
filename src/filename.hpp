// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// What each file in a database directory is called, and how to tell.
//
// Names are the only index into the directory: recovery lists it, works out
// what each file is from its name alone, and decides what to keep.  So the
// naming has to be unambiguous in both directions -- every file this engine
// writes must parse, and nothing it did not write may parse as something it
// did.  A file it cannot identify is left alone rather than deleted, because
// the directory might not be exclusively ours.

#ifndef AMBAR_FILENAME_HPP_
#define AMBAR_FILENAME_HPP_

#include <cstdint>
#include <string>
#include <string_view>

#include "ambar/status.hpp"

namespace ambar {

enum class FileType {
  kLog,
  kLock,
  kTable,
  kDescriptor,  // a manifest
  kCurrent,
  kTemp,
  kInfoLog,
};

std::string log_file_name(const std::string& dbname, uint64_t number);
std::string table_file_name(const std::string& dbname, uint64_t number);
std::string descriptor_file_name(const std::string& dbname, uint64_t number);
std::string current_file_name(const std::string& dbname);
std::string lock_file_name(const std::string& dbname);
std::string temp_file_name(const std::string& dbname, uint64_t number);
std::string info_log_file_name(const std::string& dbname);

// Points CURRENT at MANIFEST-<number>.
//
// Written to a temporary file and renamed, because rename is atomic and a
// direct write is not: a crash halfway through writing CURRENT would leave a
// file naming no manifest at all, and the database would refuse to open with
// every one of its files intact.
Status set_current_file(const std::string& dbname, uint64_t descriptor_number);

// Parses `filename` (the base name, not a path).  Returns false for anything
// this engine did not write.
bool parse_file_name(std::string_view filename, uint64_t* number,
                     FileType* type);

}  // namespace ambar

#endif  // AMBAR_FILENAME_HPP_
