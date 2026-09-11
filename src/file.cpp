// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "file.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>

#if defined(_WIN32)
// Trimmed, and with the min/max macros suppressed: they are function-like
// macros with very common names and they break std::min and std::max at every
// later include site.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ambar {
namespace {

std::string errno_message(const std::string& what, const std::string& path) {
  return what + " '" + path + "': " + std::strerror(errno);
}

#if defined(_WIN32)

std::string win32_message(const char* what, const std::string& path,
                          DWORD error) {
  return std::string(what) + " '" + path + "': error " + std::to_string(error);
}

bool same_file(const BY_HANDLE_FILE_INFORMATION& a,
               const BY_HANDLE_FILE_INFORMATION& b) {
  return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
         a.nFileIndexHigh == b.nFileIndexHigh &&
         a.nFileIndexLow == b.nFileIndexLow;
}

// Opens `path` without following a symbolic link or junction placed there --
// the property O_NOFOLLOW gives the POSIX branch of this file.
//
// FILE_FLAG_OPEN_REPARSE_POINT opens a reparse point *itself* rather than
// whatever it resolves to, and the handle then says what it is.  Asking the
// handle rather than the path leaves no window in which the file could be
// swapped for a link between the check and the open.
//
// Not every reparse point is a link.  A OneDrive placeholder, a file
// compressed with `compact /exe`, a deduplicated one: each carries the
// attribute, none redirects anywhere, and each is unreadable through a handle
// that bypassed its filter driver.  Those are reopened the ordinary way and
// then shown to be the same file as the one just inspected.
//
// `create_error` receives the error from whichever CreateFile failed, or
// zero, so a caller can tell a sharing violation from the rest.
Status open_refusing_links(const std::string& path, DWORD desired_access,
                           DWORD share, DWORD creation, const char* what,
                           DWORD* create_error, HANDLE* out) {
  *create_error = 0;
  HANDLE handle = ::CreateFileA(
      path.c_str(), desired_access, share, nullptr, creation,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *create_error = ::GetLastError();
    return Status::io_error(win32_message(what, path, *create_error));
  }

  FILE_ATTRIBUTE_TAG_INFO tag;
  if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                      sizeof(tag))) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    return Status::io_error(win32_message(what, path, error));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
    *out = handle;  // a plain file, which is nearly always the case
    return Status::ok();
  }
  if (IsReparseTagNameSurrogate(tag.ReparseTag)) {
    ::CloseHandle(handle);
    return Status::io_error(std::string(what) + " '" + path +
                            "': it is a symbolic link or junction, which is "
                            "refused rather than followed");
  }

  BY_HANDLE_FILE_INFORMATION inspected;
  if (!::GetFileInformationByHandle(handle, &inspected)) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    return Status::io_error(win32_message(what, path, error));
  }
  ::CloseHandle(handle);

  handle = ::CreateFileA(path.c_str(), desired_access, share, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    *create_error = ::GetLastError();
    return Status::io_error(win32_message(what, path, *create_error));
  }
  BY_HANDLE_FILE_INFORMATION reopened;
  if (!::GetFileInformationByHandle(handle, &reopened)) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    return Status::io_error(win32_message(what, path, error));
  }
  if (!same_file(inspected, reopened)) {
    ::CloseHandle(handle);
    return Status::io_error(std::string(what) + " '" + path +
                            "': the file changed underneath the open");
  }
  *out = handle;
  return Status::ok();
}

// Hands an open handle to the C runtime as a FILE*, which WritableFile and
// SequentialFile are built on.  The runtime owns the handle from here on:
// fclose closes it.
Status stream_from_handle(HANDLE handle, int crt_flags, const char* mode,
                          const std::string& path, const char* what,
                          std::FILE** out) {
  const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                   crt_flags | _O_BINARY);
  if (fd < 0) {
    const std::string message = errno_message(what, path);
    ::CloseHandle(handle);
    return Status::io_error(message);
  }
  std::FILE* file = ::_fdopen(fd, mode);
  if (file == nullptr) {
    const std::string message = errno_message(what, path);
    ::_close(fd);
    return Status::io_error(message);
  }
  *out = file;
  return Status::ok();
}

#endif

}  // namespace

// ---------------------------------------------------------- WritableFile ---
Status WritableFile::open(const std::string& path, bool append,
                          std::unique_ptr<WritableFile>* out) {
#if defined(_WIN32)
  // OPEN_ALWAYS rather than CREATE_ALWAYS: a link at the path has to be seen
  // before anything is truncated, so a fresh file is truncated by hand once
  // the handle is known to be a plain file.  The sharing mode is the one the
  // C runtime's fopen used here before, so nothing else changes.
  HANDLE handle = INVALID_HANDLE_VALUE;
  DWORD create_error = 0;
  Status status = open_refusing_links(
      path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
      "cannot open for writing", &create_error, &handle);
  if (!status.is_ok()) return status;

  std::FILE* file = nullptr;
  status = stream_from_handle(handle, append ? _O_APPEND : 0,
                              append ? "ab" : "wb", path,
                              "cannot open for writing", &file);
  if (!status.is_ok()) return status;

  if (!append && ::_chsize_s(::_fileno(file), 0) != 0) {
    const std::string message = errno_message("cannot truncate", path);
    std::fclose(file);
    return Status::io_error(message);
  }
#else
  // O_NOFOLLOW refuses to open a symlink at this exact path.  A database
  // directory an attacker can write into can otherwise hold a symlink planted
  // where Ambar is about to create a table, log, or manifest file, redirecting
  // the write to whatever the link points at; O_CREAT still creates a normal
  // file when nothing is there yet, which is the ordinary case.  Mode 0600
  // keeps the file unreadable by anyone but its owner regardless of umask,
  // since these files hold the database's actual keys and values.
  const int flags = O_WRONLY | O_CREAT | O_NOFOLLOW |
                    (append ? O_APPEND : O_TRUNC);
  const int fd = ::open(path.c_str(), flags, 0600);
  if (fd < 0) {
    return Status::io_error(errno_message("cannot open for writing", path));
  }
  std::FILE* file = ::fdopen(fd, append ? "ab" : "wb");
  if (file == nullptr) {
    const std::string message =
        errno_message("cannot open for writing", path);
    ::close(fd);
    return Status::io_error(message);
  }
#endif
  out->reset(new WritableFile(file, path));
  return Status::ok();
}

WritableFile::~WritableFile() {
  if (file_ != nullptr) {
    std::fclose(file_);
  }
}

Status WritableFile::append(std::string_view data) {
  if (file_ == nullptr) {
    return Status::io_error("append to closed file " + path_);
  }
  const size_t written = std::fwrite(data.data(), 1, data.size(), file_);
  if (written != data.size()) {
    return Status::io_error(errno_message("short write to", path_));
  }
  bytes_written_ += written;
  return Status::ok();
}

Status WritableFile::flush() {
  if (file_ == nullptr) return Status::ok();
  if (std::fflush(file_) != 0) {
    return Status::io_error(errno_message("cannot flush", path_));
  }
  return Status::ok();
}

Status WritableFile::sync() {
  if (file_ == nullptr) return Status::ok();
  if (Status s = flush(); !s.is_ok()) return s;

#if defined(_WIN32)
  if (_commit(_fileno(file_)) != 0) {
    return Status::io_error(errno_message("cannot sync", path_));
  }
#elif defined(__APPLE__)
  // On macOS plain fsync only pushes to the drive cache; F_FULLFSYNC is the
  // call that actually waits for the platter/flash.  Using fsync here would
  // make the durability contract a lie on that platform.
  if (fcntl(fileno(file_), F_FULLFSYNC) == -1) {
    if (fsync(fileno(file_)) != 0) {
      return Status::io_error(errno_message("cannot sync", path_));
    }
  }
#else
  if (fsync(fileno(file_)) != 0) {
    return Status::io_error(errno_message("cannot sync", path_));
  }
#endif
  return Status::ok();
}

Status WritableFile::close() {
  if (file_ == nullptr) return Status::ok();
  const int result = std::fclose(file_);
  file_ = nullptr;
  if (result != 0) {
    return Status::io_error(errno_message("cannot close", path_));
  }
  return Status::ok();
}

// -------------------------------------------------------- SequentialFile ---
Status SequentialFile::open(const std::string& path,
                            std::unique_ptr<SequentialFile>* out) {
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  DWORD create_error = 0;
  Status status = open_refusing_links(
      path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING,
      "cannot open for reading", &create_error, &handle);
  if (!status.is_ok()) return status;

  std::FILE* file = nullptr;
  status = stream_from_handle(handle, _O_RDONLY, "rb", path,
                              "cannot open for reading", &file);
  if (!status.is_ok()) return status;
#else
  // Same symlink refusal as WritableFile::open, for the read side: CURRENT,
  // a MANIFEST, and a log file are all opened by a predictable name inside a
  // directory that may not be trusted.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return Status::io_error(errno_message("cannot open for reading", path));
  }
  std::FILE* file = ::fdopen(fd, "rb");
  if (file == nullptr) {
    const std::string message =
        errno_message("cannot open for reading", path);
    ::close(fd);
    return Status::io_error(message);
  }
#endif
  out->reset(new SequentialFile(file, path));
  return Status::ok();
}

SequentialFile::~SequentialFile() {
  if (file_ != nullptr) std::fclose(file_);
}

Status SequentialFile::read(size_t n, std::string_view* result,
                            std::string* scratch) {
  scratch->resize(n);
  const size_t read_bytes = std::fread(scratch->data(), 1, n, file_);
  if (read_bytes < n && std::ferror(file_) != 0) {
    return Status::io_error(errno_message("cannot read", path_));
  }
  *result = std::string_view(scratch->data(), read_bytes);
  return Status::ok();
}

// ------------------------------------------------------ RandomAccessFile ---
RandomAccessFile::~RandomAccessFile() = default;

namespace {

#if defined(_WIN32)
class LocalRandomAccessFile final : public RandomAccessFile {
 public:
  LocalRandomAccessFile(void* handle, std::string path)
      : handle_(handle), path_(std::move(path)) {}
  ~LocalRandomAccessFile() override;
  Status read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override;

 private:
  void* handle_;
  std::string path_;
};
#else
class LocalRandomAccessFile final : public RandomAccessFile {
 public:
  LocalRandomAccessFile(int fd, std::string path)
      : fd_(fd), path_(std::move(path)) {}
  ~LocalRandomAccessFile() override;
  Status read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override;

 private:
  int fd_;
  std::string path_;
};
#endif

#if defined(_WIN32)

Status open_impl(const std::string& path,
                 std::unique_ptr<RandomAccessFile>* out) {
  HANDLE handle = INVALID_HANDLE_VALUE;
  DWORD create_error = 0;
  const Status status = open_refusing_links(
      path, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
      "cannot open for reading", &create_error, &handle);
  if (!status.is_ok()) return status;
  out->reset(new LocalRandomAccessFile(handle, path));
  return Status::ok();
}

LocalRandomAccessFile::~LocalRandomAccessFile() {
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
  }
}

Status LocalRandomAccessFile::read(uint64_t offset, size_t n,
                                   std::string_view* result, char* scratch) const {
  // The offset travels in the OVERLAPPED structure rather than in the handle's
  // own file pointer, which is what makes concurrent reads on one handle safe.
  size_t done = 0;
  while (done < n) {
    OVERLAPPED overlapped = {};
    const uint64_t at = offset + done;
    overlapped.Offset = static_cast<DWORD>(at & 0xffffffffu);
    overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);

    const size_t want_bytes = n - done;
    const DWORD want = static_cast<DWORD>(
        want_bytes > 0xffffffffu ? 0xffffffffu : want_bytes);
    DWORD got = 0;
    if (!::ReadFile(static_cast<HANDLE>(handle_), scratch + done, want, &got,
                    &overlapped)) {
      const DWORD err = ::GetLastError();
      if (err == ERROR_HANDLE_EOF) break;
      return Status::io_error("read '" + path_ + "': error " +
                              std::to_string(err));
    }
    if (got == 0) break;  // end of file
    done += got;
  }
  if (done != n) {
    return Status::corruption("short read in '" + path_ + "': wanted " +
                              std::to_string(n) + " bytes at offset " +
                              std::to_string(offset) + ", got " +
                              std::to_string(done));
  }
  *result = std::string_view(scratch, n);
  return Status::ok();
}

#else

Status open_impl(const std::string& path,
                 std::unique_ptr<RandomAccessFile>* out) {
  // See WritableFile::open: a table file is opened by a predictable name in a
  // directory that may not be trusted, so a symlink planted there is refused
  // rather than followed.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return Status::io_error(errno_message("cannot open for reading", path));
  }
  out->reset(new LocalRandomAccessFile(fd, path));
  return Status::ok();
}

LocalRandomAccessFile::~LocalRandomAccessFile() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Status LocalRandomAccessFile::read(uint64_t offset, size_t n,
                                   std::string_view* result, char* scratch) const {
  // pread, not lseek + read: the offset is an argument, so the file carries no
  // shared position for two threads to move under one another.
  //
  // The loop is not defensive padding.  A single pread may return fewer bytes
  // than asked for -- it is only guaranteed to return at least one byte before
  // the end of the file -- and it is interrupted by any signal that arrives
  // mid-call.  Both are rare enough never to appear in testing and common
  // enough to appear in production.
  size_t done = 0;
  while (done < n) {
    const ssize_t got = ::pread(fd_, scratch + done, n - done,
                                static_cast<off_t>(offset + done));
    if (got < 0) {
      if (errno == EINTR) continue;
      return Status::io_error(errno_message("read", path_));
    }
    if (got == 0) break;  // end of file
    done += static_cast<size_t>(got);
  }
  if (done != n) {
    return Status::corruption("short read in '" + path_ + "': wanted " +
                              std::to_string(n) + " bytes at offset " +
                              std::to_string(offset) + ", got " +
                              std::to_string(done));
  }
  *result = std::string_view(scratch, n);
  return Status::ok();
}

#endif

}  // namespace

Status RandomAccessFile::open(const std::string& path,
                              std::unique_ptr<RandomAccessFile>* out) {
  return open_impl(path, out);
}

// ------------------------------------------------------------- FileLock ---
#if defined(_WIN32)

Status FileLock::acquire(const std::string& path,
                         std::unique_ptr<FileLock>* out) {
  // No sharing at all: the second opener fails outright, which is the
  // behaviour wanted here.
  HANDLE handle = INVALID_HANDLE_VALUE;
  DWORD create_error = 0;
  const Status status = open_refusing_links(
      path, GENERIC_READ | GENERIC_WRITE, /*share=*/0, OPEN_ALWAYS,
      "cannot create the lock file", &create_error, &handle);
  if (!status.is_ok()) {
    if (create_error == ERROR_SHARING_VIOLATION ||
        create_error == ERROR_LOCK_VIOLATION) {
      return Status::io_error("the database at '" + path +
                              "' is already open in another process");
    }
    return status;
  }
  out->reset(new FileLock(handle, path));
  return Status::ok();
}

FileLock::~FileLock() {
  if (handle_ != nullptr) ::CloseHandle(static_cast<HANDLE>(handle_));
}

#else

Status FileLock::acquire(const std::string& path,
                         std::unique_ptr<FileLock>* out) {
  // O_NOFOLLOW and 0600 for the same reasons as WritableFile::open: LOCK is a
  // predictable name in a directory that may not be trusted, and there is no
  // reason for anyone but the owner to see it exists.
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return Status::io_error(errno_message("cannot create the lock file", path));
  }

  // fcntl rather than flock: fcntl locks are the ones NFS honours, and a
  // database directory on a network filesystem is exactly the case where two
  // machines opening it at once is plausible.
  struct flock lock = {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;  // the whole file

  if (::fcntl(fd, F_SETLK, &lock) == -1) {
    const int saved = errno;
    ::close(fd);
    if (saved == EACCES || saved == EAGAIN) {
      return Status::io_error("the database at '" + path +
                              "' is already open in another process");
    }
    errno = saved;
    return Status::io_error(errno_message("cannot lock", path));
  }

  out->reset(new FileLock(fd, path));
  return Status::ok();
}

FileLock::~FileLock() {
  // Closing releases the lock.  The file itself is left behind: removing it
  // would let a second process create and lock a *new* file with the same name
  // while a third still held the old one.
  if (fd_ >= 0) ::close(fd_);
}

#endif

// ------------------------------------------------------------- utilities ---
Status file_size(const std::string& path, uint64_t* size) {
  std::error_code ec;
  const auto value = std::filesystem::file_size(path, ec);
  if (ec) return Status::io_error("cannot stat '" + path + "': " + ec.message());
  *size = static_cast<uint64_t>(value);
  return Status::ok();
}

Status remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) return Status::io_error("cannot remove '" + path + "': " + ec.message());
  return Status::ok();
}

Status rename_file(const std::string& from, const std::string& to) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    return Status::io_error("cannot rename '" + from + "' to '" + to +
                            "': " + ec.message());
  }
  return Status::ok();
}

bool file_exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

Status create_directory(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return Status::io_error("cannot create '" + path + "': " + ec.message());
  }
  return Status::ok();
}

Status sync_directory(const std::string& path) {
#if defined(_WIN32)
  (void)path;  // Windows offers no handle to a directory's metadata here.
  return Status::ok();
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::io_error(errno_message("cannot open directory", path));
  }
  const int result = fsync(fd);
  ::close(fd);
  if (result != 0) {
    return Status::io_error(errno_message("cannot sync directory", path));
  }
  return Status::ok();
#endif
}

}  // namespace ambar
