// Copyright (c) 2026 Onur Kutan.  MIT licence; see LICENSE.

#include "internal_filter_policy.hpp"

#include "dbformat.hpp"

namespace ambar {
namespace {

class InternalFilterPolicy final : public FilterPolicy {
 public:
  explicit InternalFilterPolicy(const FilterPolicy* user_policy)
      : user_policy_(user_policy) {}

  const char* name() const override { return user_policy_->name(); }

  void create_filter(const std::vector<std::string_view>& keys,
                     std::string* dst) const override {
    std::vector<std::string_view> user_keys;
    user_keys.reserve(keys.size());
    for (const std::string_view key : keys) {
      // A key too short to hold a trailer cannot have come from this engine.
      // It is passed through whole rather than dropped: a filter with an extra
      // entry costs a read, a filter with a missing entry loses the key.
      user_keys.push_back(key.size() >= 8 ? extract_user_key(key) : key);
    }
    user_policy_->create_filter(user_keys, dst);
  }

  bool key_may_match(std::string_view key,
                     std::string_view filter) const override {
    return user_policy_->key_may_match(
        key.size() >= 8 ? extract_user_key(key) : key, filter);
  }

 private:
  const FilterPolicy* const user_policy_;
};

}  // namespace

std::unique_ptr<const FilterPolicy> new_internal_filter_policy(
    const FilterPolicy* user_policy) {
  return std::make_unique<InternalFilterPolicy>(user_policy);
}

}  // namespace ambar
