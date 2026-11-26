// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_CHARSET_POLICY_H_
#define RIME_CHARSET_POLICY_H_

#include <bitset>
#include <cstdint>
#include <rime/common.h>
#include <rime/dict/vocabulary.h>

namespace rime {

class Config;
class Context;
class RewriteResource;
struct Segment;

// Opt-in policy for system dictionary entries. Only single Han characters
// are restricted; lexical phrases and user dictionary entries are exempt.
class RIME_DLL CharsetPolicy {
 public:
  static an<CharsetPolicy> Load(Config* config, const string& name_space);
  // Empty function means unrestricted. A query pins one resource snapshot
  // and shares its character cache across dictionary iterators.
  DictTextFilter Bind(const Context* context,
                      const Segment* segment = nullptr) const;

 private:
  struct Rule {
    vector<string> options;
    std::bitset<256> base;
    hash_set<uint32_t> add;
    hash_set<uint32_t> ban;
  };
  an<RewriteResource> resource_;
  vector<Rule> rules_;
  vector<string> exclude_tags_;
};

}  // namespace rime

#endif  // RIME_CHARSET_POLICY_H_
