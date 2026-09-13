// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "sim_file_system.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace ambar {

namespace {

// What stdio buffers before it hands bytes to the kernel on its own.  A
// record larger than this reaches the kernel in pieces before any flush,
// and the model has to allow that a piece landed.
constexpr size_t kUserBuffer = 4096;
constexpr uint64_t kPage = 4096;

Status power_off(const char* what, const std::string& path) {
  return Status::io_error(std::string("power is off: cannot ") + what + " '" +
                          path + "'");
}

// How much of `n` pending units landed.  Nothing and everything are the
// common outcomes of a real cut and are drawn a third of the time each,
// so that a sweep visits them without needing many draws.
uint64_t landed(std::mt19937_64& rng, uint64_t n) {
  if (n == 0) return 0;
  switch (rng() % 3) {
    case 0:
      return 0;
    case 1:
      return n;
    default:
      return rng() % (n + 1);
  }
}

}  // namespace

// ------------------------------------------------------------- handles ---

class SimWritableFile final : public WritableFile {
 public:
  SimWritableFile(SimFileSystem* fs, SimFileSystem::FilePtr file,
                  std::string path)
      : fs_(fs),
        file_(std::move(file)),
        path_(std::move(path)),
        generation_(fs->generation_) {}

  ~SimWritableFile() override {
    // fclose flushes what the process still holds, so a dropped handle
    // does too -- unless the power has gone, when nothing runs.
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (closed_ || !alive()) return;
    if (fs_->begin_operation("close", path_).is_ok()) {
      file_->kernel += pending_;
      pending_.clear();
    }
  }

  Status append(std::string_view data) override {
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (closed_) return Status::io_error("append to closed file " + path_);
    if (!alive()) return power_off("append to", path_);
    Status status = fs_->begin_operation("append", path_);
    if (!status.is_ok()) {
      // fwrite that fails discards the stream's buffer along with the
      // bytes it was given; so does a failed fflush, below.
      if (fs_->powered_) pending_.clear();
      return status;
    }
    pending_.append(data.data(), data.size());
    while (pending_.size() >= kUserBuffer) {
      file_->kernel.append(pending_, 0, kUserBuffer);
      pending_.erase(0, kUserBuffer);
    }
    return Status::ok();
  }

  Status flush() override {
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (closed_) return Status::ok();
    if (!alive()) return power_off("flush", path_);
    Status status = fs_->begin_operation("flush", path_);
    if (!status.is_ok()) {
      if (fs_->powered_) pending_.clear();
      return status;
    }
    file_->kernel += pending_;
    pending_.clear();
    return Status::ok();
  }

  Status sync() override {
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (closed_) return Status::ok();
    if (!alive()) return power_off("sync", path_);
    // The flush inside a sync comes first, as in the real one, so a sync
    // that fails has still handed its bytes to the kernel.
    file_->kernel += pending_;
    pending_.clear();
    Status status = fs_->begin_operation("sync", path_);
    if (!status.is_ok()) {
      if (fs_->powered_ && file_->durable < file_->kernel.size()) {
        // A failed fsync: the dirty pages are marked clean and stay in the
        // cache, readable, and nothing will ever write them.
        file_->lost.emplace_back(file_->durable, file_->kernel.size());
      }
      return status;
    }
    file_->durable = file_->kernel.size();
    file_->before.reset();  // the truncation, if any, is on the disk now
    if (fs_->model_.dirents == SimFileSystem::Dirents::kFile) {
      fs_->commit_names_of(file_);
    }
    return Status::ok();
  }

  Status close() override {
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (closed_) return Status::ok();
    if (!alive()) return power_off("close", path_);
    Status status = fs_->begin_operation("close", path_);
    if (!status.is_ok()) {
      // fclose that fails still closes the stream, and whatever it had not
      // flushed is gone with it.
      if (fs_->powered_) {
        closed_ = true;
        pending_.clear();
      }
      return status;
    }
    file_->kernel += pending_;
    pending_.clear();
    closed_ = true;
    return Status::ok();
  }

 private:
  bool alive() const {
    return fs_->powered_ && generation_ == fs_->generation_;
  }

  SimFileSystem* fs_;
  SimFileSystem::FilePtr file_;
  std::string path_;
  uint64_t generation_;
  std::string pending_;  // this process's own buffer: can never land
  bool closed_ = false;
};

class SimSequentialFile final : public SequentialFile {
 public:
  SimSequentialFile(SimFileSystem* fs, SimFileSystem::FilePtr file,
                    std::string path)
      : fs_(fs),
        file_(std::move(file)),
        path_(std::move(path)),
        generation_(fs->generation_) {}

  Status read(size_t n, std::string_view* result,
              std::string* scratch) override {
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (!fs_->powered_ || generation_ != fs_->generation_) {
      return power_off("read", path_);
    }
    const size_t available =
        position_ < file_->kernel.size() ? file_->kernel.size() - position_ : 0;
    const size_t count = std::min(n, available);
    scratch->assign(file_->kernel, position_, count);
    position_ += count;
    *result = std::string_view(*scratch);
    return Status::ok();
  }

 private:
  SimFileSystem* fs_;
  SimFileSystem::FilePtr file_;
  std::string path_;
  uint64_t generation_;
  size_t position_ = 0;
};

class SimRandomAccessFile final : public RandomAccessFile {
 public:
  SimRandomAccessFile(SimFileSystem* fs, SimFileSystem::FilePtr file,
                      std::string path)
      : fs_(fs),
        file_(std::move(file)),
        path_(std::move(path)),
        generation_(fs->generation_) {}

  Status read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (!fs_->powered_ || generation_ != fs_->generation_) {
      return power_off("read", path_);
    }
    const uint64_t size = file_->kernel.size();
    const uint64_t got = offset < size ? std::min<uint64_t>(n, size - offset)
                                       : 0;
    if (got != n) {
      return Status::corruption("short read in '" + path_ + "': wanted " +
                                std::to_string(n) + " bytes at offset " +
                                std::to_string(offset) + ", got " +
                                std::to_string(got));
    }
    std::memcpy(scratch, file_->kernel.data() + offset, n);
    *result = std::string_view(scratch, n);
    return Status::ok();
  }

 private:
  SimFileSystem* fs_;
  SimFileSystem::FilePtr file_;
  std::string path_;
  uint64_t generation_;
};

class SimFileLock final : public FileLock {
 public:
  SimFileLock(SimFileSystem* fs, std::string path)
      : fs_(fs), path_(std::move(path)), generation_(fs->generation_) {}

  ~SimFileLock() override {
    // A lock the dead process held was released by the cut; a handle that
    // outlived it must not release the one a later process took.
    std::lock_guard<std::mutex> lock(fs_->mutex_);
    if (generation_ == fs_->generation_) fs_->locked_.erase(path_);
  }

 private:
  SimFileSystem* fs_;
  std::string path_;
  uint64_t generation_;
};

// ---------------------------------------------------------- SimFileSystem ---

SimFileSystem::SimFileSystem(Model model, uint64_t seed)
    : model_(model), seed_(seed) {}

SimFileSystem::~SimFileSystem() = default;

uint64_t SimFileSystem::operations() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return operations_;
}

uint64_t SimFileSystem::count(const std::string& what) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = counts_.find(what);
  return it == counts_.end() ? 0 : it->second;
}

std::map<std::string, uint64_t> SimFileSystem::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

void SimFileSystem::forget() {
  std::lock_guard<std::mutex> lock(mutex_);
  cut_at_.clear();
  cut_report_.clear();
  last_fault_.clear();
}

void SimFileSystem::crash_at(uint64_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  crash_at_ = index;
}

void SimFileSystem::crash() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!powered_) return;
  cut_at_ = "the end of the workload";
  cut();
}

void SimFileSystem::fail_at(uint64_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  fail_at_ = index;
}

void SimFileSystem::fail_at(const std::string& kind, uint64_t ordinal) {
  std::lock_guard<std::mutex> lock(mutex_);
  fail_kind_ = kind;
  fail_ordinal_ = ordinal;
}

void SimFileSystem::crash_after_fault(uint64_t operations) {
  std::lock_guard<std::mutex> lock(mutex_);
  crash_after_fault_ = operations;
}

uint64_t SimFileSystem::faults() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return faults_;
}

bool SimFileSystem::powered() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return powered_;
}

void SimFileSystem::power_on() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (powered_) return;
  live_ = survived_;
  committed_ = live_;
  survived_ = Tree();
  pending_.clear();
  locked_.clear();
  ++generation_;
  powered_ = true;
}

std::string SimFileSystem::directory_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// The kind of file a path names, for the counters: an extension, or the
// name itself for the files that have none.
std::string SimFileSystem::kind_of(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const std::string base =
      slash == std::string::npos ? path : path.substr(slash + 1);
  if (base.compare(0, 9, "MANIFEST-") == 0) return "MANIFEST";
  const size_t dot = base.find_last_of('.');
  return dot == std::string::npos ? base : base.substr(dot);
}

Status SimFileSystem::begin_operation(const char* what,
                                      const std::string& path,
                                      bool directory) {
  if (!powered_) return power_off(what, path);
  if (operations_ == crash_at_) {
    cut_at_ = std::string(what) + " " + path + " (operation " +
              std::to_string(operations_) + ")";
    cut();
    return power_off(what, path);
  }
  const std::string key =
      directory ? std::string(what) : std::string(what) + " " + kind_of(path);
  const bool by_kind = key == fail_kind_ && counts_[key] == fail_ordinal_;
  if (operations_ == fail_at_ || by_kind) {
    last_fault_ = key + " " + path + " (operation " +
                  std::to_string(operations_) + ", ordinal " +
                  std::to_string(counts_[key]) + " of its kind)";
    fail_at_ = kNever;
    fail_kind_.clear();
    ++faults_;
    ++operations_;
    if (crash_after_fault_ != kNever) {
      crash_at_ = operations_ + crash_after_fault_;
      crash_after_fault_ = kNever;
    }
    return Status::io_error(std::string("injected fault: cannot ") + what +
                            " '" + path + "'");
  }
  ++operations_;
  ++counts_[key];
  return Status::ok();
}

// A journal commit: every pending name lands.  A pending truncation does
// not -- that is the file's own, and lands with the file's own sync.
void SimFileSystem::commit_all() {
  committed_ = live_;
  pending_.clear();
}

// Syncing a file commits the names that refer to it -- its creation, a
// rename that moved it -- and nothing else.  The rest of the pending list
// stays pending, in order, except what those names cannot land without: the
// directory a name is in, if its creation is still pending, and whatever
// vacated the name first -- a removal, a rename away -- since a journal
// never lands the second of those without the first.
void SimFileSystem::commit_names_of(const FilePtr& file) {
  std::vector<bool> commit(pending_.size(), false);
  for (size_t i = 0; i < pending_.size(); ++i) {
    const Event& event = pending_[i];
    if (event.kind == Event::kMkdir || event.file != file) continue;
    commit[i] = true;
    const std::string name =
        event.kind == Event::kRename ? event.to : event.path;
    const std::string parent = directory_of(name);
    for (size_t j = 0; j < i; ++j) {
      const Event& earlier = pending_[j];
      switch (earlier.kind) {
        case Event::kMkdir: {
          const std::string& made = earlier.path;
          if (made == parent ||
              parent.compare(0, made.size() + 1, made + "/") == 0) {
            commit[j] = true;
          }
          break;
        }
        case Event::kDelete:
        case Event::kCreate:
          if (earlier.path == name) commit[j] = true;
          break;
        case Event::kRename:
          if (earlier.path == name || earlier.to == name) commit[j] = true;
          break;
      }
    }
  }
  std::vector<Event> remaining;
  for (size_t i = 0; i < pending_.size(); ++i) {
    if (commit[i]) {
      replay(&committed_, pending_[i]);
    } else {
      remaining.push_back(pending_[i]);
    }
  }
  pending_ = std::move(remaining);
}

void SimFileSystem::replay(Tree* tree, const Event& event) {
  switch (event.kind) {
    case Event::kCreate:
      tree->files[event.path] = event.file;
      break;
    case Event::kRename:
      tree->files.erase(event.path);
      tree->files[event.to] = event.file;
      break;
    case Event::kDelete:
      tree->files.erase(event.path);
      tree->directories.erase(event.path);
      break;
    case Event::kMkdir:
      tree->directories.insert(event.path);
      break;
  }
}

void SimFileSystem::cut() {
  // Seeded per cut, so a failure names a seed and a cut number that
  // reproduce its choices -- given the same sequence of operations, which
  // a background compaction can vary.
  std::mt19937_64 rng(seed_ * 1000003 + crashes_);
  ++crashes_;
  crash_at_ = kNever;

  // Names: the committed tree plus a prefix of what was pending.
  Tree tree = committed_;
  const uint64_t events = landed(rng, pending_.size());
  for (uint64_t i = 0; i < events; ++i) replay(&tree, pending_[i]);

  // Data: each file's synced prefix plus some of its unsynced tail -- or,
  // for a file truncated since its last sync, possibly what it held before.
  survived_ = Tree();
  survived_.directories = tree.directories;
  cut_report_.clear();
  cut_report_.push_back(std::to_string(events) + " of " +
                        std::to_string(pending_.size()) +
                        " pending name change(s) landed");
  for (const auto& [path, file] : tree.files) {
    const std::string* kernel = &file->kernel;
    const std::vector<std::pair<uint64_t, uint64_t>>* lost = &file->lost;
    uint64_t synced = file->durable;
    const bool old = file->before && (rng() & 1) != 0;
    if (old) {
      kernel = &file->before->kernel;
      lost = &file->before->lost;
      synced = file->before->durable;
    }
    const uint64_t size = kernel->size();
    synced = std::min(synced, size);
    const uint64_t extra = landed(rng, size - synced);
    cut_report_.push_back(path + ": had " + std::to_string(size) +
                          " bytes, " + std::to_string(synced) +
                          " synced; kept " + std::to_string(extra) +
                          " of the " + std::to_string(size - synced) +
                          " unsynced" + (old ? " (before its truncation)" : ""));
    std::string content = kernel->substr(0, synced + extra);
    if (model_.tails != Tails::kOrdered && extra > 0) {
      // Pages of the tail, each kept or not on its own; never a byte
      // below `synced`, however the page boundaries fall.
      for (uint64_t page = synced / kPage * kPage; page < synced + extra;
           page += kPage) {
        if ((rng() & 1) == 0) continue;
        const uint64_t from = std::max(page, synced);
        const uint64_t to = std::min(page + kPage, synced + extra);
        for (uint64_t i = from; i < to; ++i) {
          content[static_cast<size_t>(i)] =
              model_.tails == Tails::kHoles ? '\0'
                                            : static_cast<char>(rng() & 0xff);
        }
      }
    }
    for (const auto& [from, to] : *lost) {
      const uint64_t stop = std::min<uint64_t>(to, content.size());
      if (from >= stop) continue;
      cut_report_.push_back("  " + path + ": " +
                            std::to_string(stop - from) +
                            " bytes a failed sync never wrote read as " +
                            (model_.tails == Tails::kGarbage ? "garbage"
                                                             : "zeros"));
      for (uint64_t i = from; i < stop; ++i) {
        content[static_cast<size_t>(i)] =
            model_.tails == Tails::kGarbage ? static_cast<char>(rng() & 0xff)
                                            : '\0';
      }
    }
    auto survivor = std::make_shared<File>();
    survivor->durable = content.size();
    survivor->kernel = std::move(content);
    survived_.files[path] = survivor;
  }
  powered_ = false;
}

// The handles are assigned to *out after the lock is released: resetting a
// unique_ptr that still holds an old handle runs its destructor, which
// takes the lock itself.
Status SimFileSystem::new_writable_file(const std::string& path, bool append,
                                        std::unique_ptr<WritableFile>* out) {
  std::unique_ptr<WritableFile> handle;
  std::unique_lock<std::mutex> lock(mutex_);
  if (!powered_) return power_off("create", path);
  if (live_.directories.count(directory_of(path)) == 0) {
    return Status::io_error("cannot open for writing '" + path +
                            "': no such directory");
  }
  if (live_.directories.count(path) != 0) {
    return Status::io_error("cannot open for writing '" + path +
                            "': it is a directory");
  }
  auto existing = live_.files.find(path);
  FilePtr file;
  if (existing != live_.files.end()) {
    file = existing->second;  // the same inode, whether appended or truncated
    if (!append) {
      Status status = begin_operation("truncate", path);
      if (!status.is_ok()) return status;
      if (!file->before) {
        file->before = File::Before{file->kernel, file->durable, file->lost};
      }
      file->kernel.clear();
      file->durable = 0;
      file->lost.clear();
    }
  } else {
    Status status = begin_operation("create", path);
    if (!status.is_ok()) return status;
    file = std::make_shared<File>();
    live_.files[path] = file;
    pending_.push_back(Event{Event::kCreate, path, std::string(), file});
  }
  handle.reset(new SimWritableFile(this, file, path));
  lock.unlock();
  *out = std::move(handle);
  return Status::ok();
}

Status SimFileSystem::new_sequential_file(
    const std::string& path, std::unique_ptr<SequentialFile>* out) {
  std::unique_ptr<SequentialFile> handle;
  std::unique_lock<std::mutex> lock(mutex_);
  if (!powered_) return power_off("open", path);
  auto it = live_.files.find(path);
  if (it == live_.files.end()) {
    return Status::io_error("cannot open for reading '" + path +
                            "': no such file");
  }
  handle.reset(new SimSequentialFile(this, it->second, path));
  lock.unlock();
  *out = std::move(handle);
  return Status::ok();
}

Status SimFileSystem::new_random_access_file(
    const std::string& path, std::unique_ptr<RandomAccessFile>* out) {
  std::unique_ptr<RandomAccessFile> handle;
  std::unique_lock<std::mutex> lock(mutex_);
  if (!powered_) return power_off("open", path);
  auto it = live_.files.find(path);
  if (it == live_.files.end()) {
    return Status::io_error("cannot open for reading '" + path +
                            "': no such file");
  }
  handle.reset(new SimRandomAccessFile(this, it->second, path));
  lock.unlock();
  *out = std::move(handle);
  return Status::ok();
}

Status SimFileSystem::lock_file(const std::string& path,
                                std::unique_ptr<FileLock>* out) {
  std::unique_ptr<FileLock> handle;
  std::unique_lock<std::mutex> lock(mutex_);
  if (!powered_) return power_off("lock", path);
  if (locked_.count(path) != 0) {
    return Status::io_error("the database at '" + path +
                            "' is already open in another process");
  }
  if (live_.files.count(path) == 0) {
    if (live_.directories.count(directory_of(path)) == 0) {
      return Status::io_error("cannot create the lock file '" + path +
                              "': no such directory");
    }
    Status status = begin_operation("create", path);
    if (!status.is_ok()) return status;
    auto file = std::make_shared<File>();
    live_.files[path] = file;
    pending_.push_back(Event{Event::kCreate, path, std::string(), file});
  }
  locked_.insert(path);
  handle.reset(new SimFileLock(this, path));
  lock.unlock();
  *out = std::move(handle);
  return Status::ok();
}

Status SimFileSystem::file_size(const std::string& path, uint64_t* size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!powered_) return power_off("stat", path);
  auto it = live_.files.find(path);
  if (it == live_.files.end()) {
    return Status::io_error("cannot stat '" + path + "': no such file");
  }
  *size = it->second->kernel.size();
  return Status::ok();
}

Status SimFileSystem::remove_file(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  Status status = begin_operation("remove", path);
  if (!status.is_ok()) return status;
  if (live_.directories.count(path) != 0) {
    return Status::io_error("cannot remove '" + path + "': it is a directory");
  }
  if (live_.files.erase(path) == 0) return Status::ok();  // as remove(3)
  pending_.push_back(Event{Event::kDelete, path, std::string(), nullptr});
  return Status::ok();
}

Status SimFileSystem::rename_file(const std::string& from,
                                  const std::string& to) {
  std::lock_guard<std::mutex> lock(mutex_);
  Status status = begin_operation("rename", to);
  if (!status.is_ok()) return status;
  auto it = live_.files.find(from);
  if (it == live_.files.end()) {
    return Status::io_error("cannot rename '" + from + "' to '" + to +
                            "': no such file");
  }
  if (live_.directories.count(to) != 0 ||
      live_.directories.count(directory_of(to)) == 0) {
    return Status::io_error("cannot rename '" + from + "' to '" + to +
                            "': not a place a file can go");
  }
  FilePtr file = it->second;
  live_.files.erase(it);
  live_.files[to] = file;
  pending_.push_back(Event{Event::kRename, from, to, file});
  return Status::ok();
}

PathType SimFileSystem::path_type(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!powered_) return PathType::kMissing;
  if (live_.files.count(path) != 0) return PathType::kFile;
  if (live_.directories.count(path) != 0) return PathType::kDirectory;
  return PathType::kMissing;
}

Status SimFileSystem::create_directory(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  Status status = begin_operation("mkdir", path, /*directory=*/true);
  if (!status.is_ok()) return status;
  if (live_.files.count(path) != 0) {
    return Status::io_error("cannot create '" + path + "': it is a file");
  }
  // Every missing ancestor as well, each its own name change, as
  // create_directories does.
  std::vector<std::string> to_make;
  for (std::string dir = path; !dir.empty() &&
                                live_.directories.count(dir) == 0;
       dir = directory_of(dir)) {
    to_make.push_back(dir);
  }
  for (auto it = to_make.rbegin(); it != to_make.rend(); ++it) {
    live_.directories.insert(*it);
    pending_.push_back(Event{Event::kMkdir, *it, std::string(), nullptr});
  }
  return Status::ok();
}

Status SimFileSystem::remove_directory(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  Status status = begin_operation("rmdir", path, /*directory=*/true);
  if (!status.is_ok()) return status;
  if (live_.directories.count(path) == 0) {
    return Status::io_error("cannot remove '" + path + "': no such directory");
  }
  for (const auto& entry : live_.files) {
    if (directory_of(entry.first) == path) {
      return Status::io_error("cannot remove '" + path + "': not empty");
    }
  }
  for (const auto& directory : live_.directories) {
    if (directory_of(directory) == path) {
      return Status::io_error("cannot remove '" + path + "': not empty");
    }
  }
  live_.directories.erase(path);
  pending_.push_back(Event{Event::kDelete, path, std::string(), nullptr});
  return Status::ok();
}

Status SimFileSystem::list_directory(const std::string& path,
                                     std::vector<std::string>* names) {
  std::lock_guard<std::mutex> lock(mutex_);
  names->clear();
  if (!powered_) return power_off("list", path);
  if (live_.directories.count(path) == 0) {
    return Status::io_error("cannot list '" + path + "': no such directory");
  }
  for (const auto& entry : live_.files) {
    if (directory_of(entry.first) == path) {
      names->push_back(entry.first.substr(path.size() + 1));
    }
  }
  for (const auto& directory : live_.directories) {
    if (directory_of(directory) == path) {
      names->push_back(directory.substr(path.size() + 1));
    }
  }
  return Status::ok();
}

Status SimFileSystem::sync_directory(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  Status status =
      begin_operation("sync directory", path, /*directory=*/true);
  if (!status.is_ok()) return status;
  if (live_.directories.count(path) == 0) {
    return Status::io_error("cannot open directory '" + path +
                            "': no such directory");
  }
  commit_all();
  return Status::ok();
}

std::vector<std::string> SimFileSystem::describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> lines;
  const Tree& tree = powered_ ? live_ : survived_;
  for (const auto& [path, file] : tree.files) {
    lines.push_back(path + "  " + std::to_string(file->kernel.size()) +
                    " bytes, " + std::to_string(file->durable) + " synced");
  }
  lines.push_back((powered_ ? "power is on; " : "power is off; ") +
                  std::to_string(pending_.size()) +
                  " name change(s) not yet committed; operation " +
                  std::to_string(operations_));
  if (!last_fault_.empty()) lines.push_back("last fault at " + last_fault_);
  if (!cut_at_.empty()) {
    lines.push_back("last cut at " + cut_at_);
    for (const std::string& line : cut_report_) lines.push_back("  " + line);
  }
  return lines;
}

Status SimFileSystem::dump_to(const std::string& directory) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Tree& tree = powered_ ? live_ : survived_;
  std::error_code ec;
  const std::filesystem::path root(directory);
  for (const std::string& sub : tree.directories) {
    std::filesystem::create_directories(root / sub, ec);
    if (ec) return Status::io_error("cannot create '" + sub + "'");
  }
  for (const auto& [path, file] : tree.files) {
    const std::filesystem::path target = root / path;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    stream.write(file->kernel.data(),
                 static_cast<std::streamsize>(file->kernel.size()));
    if (!stream) return Status::io_error("cannot write '" + path + "'");
  }
  return Status::ok();
}

}  // namespace ambar
