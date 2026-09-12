// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.
//
// A returned error, not a thrown one.
//
// Storage code spends most of its life reacting to I/O that went wrong, and
// exceptions make those paths invisible at the call site.  Every operation here
// returns a Status the caller has to look at, so the error handling shows up in
// the code you read rather than in the code you don't.
#pragma once

#include <string>
#include <utility>

namespace ambar {

class Status {
 public:
  enum class Code {
    kOk = 0,
    kNotFound,
    kCorruption,   // a checksum or format check failed
    kIoError,      // the operating system said no
    kInvalidArgument,
    kNotSupported,
  };

  Status() = default;  // ok

  static Status ok() { return Status{}; }
  static Status not_found(std::string what) {
    return Status{Code::kNotFound, std::move(what)};
  }
  static Status corruption(std::string what) {
    return Status{Code::kCorruption, std::move(what)};
  }
  static Status io_error(std::string what) {
    return Status{Code::kIoError, std::move(what)};
  }
  static Status invalid_argument(std::string what) {
    return Status{Code::kInvalidArgument, std::move(what)};
  }
  static Status not_supported(std::string what) {
    return Status{Code::kNotSupported, std::move(what)};
  }

  bool is_ok() const { return code_ == Code::kOk; }
  bool is_not_found() const { return code_ == Code::kNotFound; }
  bool is_corruption() const { return code_ == Code::kCorruption; }
  bool is_io_error() const { return code_ == Code::kIoError; }
  bool is_invalid_argument() const {
    return code_ == Code::kInvalidArgument;
  }

  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string to_string() const;

  explicit operator bool() const { return is_ok(); }

 private:
  Status(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace ambar
