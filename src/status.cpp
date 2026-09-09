#include "ambar/status.hpp"

namespace ambar {

std::string Status::to_string() const {
  const char* name = "unknown";
  switch (code_) {
    case Code::kOk: return "ok";
    case Code::kNotFound: name = "not found"; break;
    case Code::kCorruption: name = "corruption"; break;
    case Code::kIoError: name = "io error"; break;
    case Code::kInvalidArgument: name = "invalid argument"; break;
    case Code::kNotSupported: name = "not supported"; break;
  }
  return message_.empty() ? std::string(name)
                          : std::string(name) + ": " + message_;
}

}  // namespace ambar
