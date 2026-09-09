// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "filename.hpp"

#include <cstdio>
#include <memory>

#include "file.hpp"

namespace ambar {
namespace {

// Parses a decimal number, refusing anything that is not one.
//
// The overflow check is the point.  A directory entry named
// "18446744073709551617.log" would otherwise wrap to 1 and be classified
// against the wrong file number in remove_obsolete_files -- deciding that a
// live file is obsolete, or the reverse.  The directory is not necessarily
// ours alone, so its contents are input.
bool parse_number(std::string_view text, uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;

  uint64_t result = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<uint64_t>(c - '0');
    if (result > (UINT64_MAX - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

std::string numbered(const std::string& dbname, uint64_t number,
                     const char* suffix) {
  char buf[64];
  // Zero-padded to six digits so a directory listing sorts in creation order,
  // which makes a database directory readable by a person without tooling.
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

}  // namespace

std::string log_file_name(const std::string& dbname, uint64_t number) {
  return numbered(dbname, number, "log");
}

std::string table_file_name(const std::string& dbname, uint64_t number) {
  return numbered(dbname, number, "sst");
}

std::string descriptor_file_name(const std::string& dbname, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string current_file_name(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string lock_file_name(const std::string& dbname) {
  return dbname + "/LOCK";
}

std::string temp_file_name(const std::string& dbname, uint64_t number) {
  return numbered(dbname, number, "dbtmp");
}

std::string info_log_file_name(const std::string& dbname) {
  return dbname + "/LOG";
}

bool parse_file_name(std::string_view filename, uint64_t* number,
                     FileType* type) {
  if (filename == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrent;
    return true;
  }
  if (filename == "LOCK") {
    *number = 0;
    *type = FileType::kLock;
    return true;
  }
  if (filename == "LOG" || filename == "LOG.old") {
    *number = 0;
    *type = FileType::kInfoLog;
    return true;
  }

  constexpr std::string_view kManifestPrefix = "MANIFEST-";
  std::string_view rest = filename;
  if (filename.substr(0, kManifestPrefix.size()) == kManifestPrefix) {
    rest.remove_prefix(kManifestPrefix.size());
    uint64_t value = 0;
    if (!parse_number(rest, &value)) return false;
    *number = value;
    *type = FileType::kDescriptor;
    return true;
  }

  const size_t dot = filename.find('.');
  if (dot == std::string_view::npos || dot == 0) return false;

  uint64_t value = 0;
  if (!parse_number(filename.substr(0, dot), &value)) return false;

  const std::string_view suffix = filename.substr(dot + 1);
  if (suffix == "log") {
    *type = FileType::kLog;
  } else if (suffix == "sst") {
    *type = FileType::kTable;
  } else if (suffix == "dbtmp") {
    *type = FileType::kTemp;
  } else {
    return false;
  }
  *number = value;
  return true;
}

Status set_current_file(const std::string& dbname,
                        uint64_t descriptor_number) {
  // Only the base name goes into CURRENT, so a database directory can be moved
  // or renamed and still open.
  const std::string manifest = descriptor_file_name(dbname, descriptor_number);
  const std::string_view base =
      std::string_view(manifest).substr(dbname.size() + 1);

  const std::string temp = temp_file_name(dbname, descriptor_number);
  Status status;
  {
    std::unique_ptr<WritableFile> file;
    status = WritableFile::open(temp, /*append=*/false, &file);
    if (status.is_ok()) status = file->append(base);
    if (status.is_ok()) status = file->append("\n");
    // Synced before the rename: a rename that lands before the contents do
    // would leave CURRENT naming a manifest while holding nothing.
    if (status.is_ok()) status = file->sync();
    if (status.is_ok()) status = file->close();
  }
  if (status.is_ok()) {
    status = rename_file(temp, current_file_name(dbname));
  }
  if (status.is_ok()) {
    // And the directory itself, so the rename survives a power cut.
    status = sync_directory(dbname);
  }
  if (!status.is_ok()) {
    remove_file(temp);
  }
  return status;
}

}  // namespace ambar
