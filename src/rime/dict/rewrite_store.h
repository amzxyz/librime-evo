//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_REWRITE_STORE_H_
#define RIME_REWRITE_STORE_H_

#include <cstdint>
#include <mutex>
#include <rime_api.h>
#include <rime/common.h>
#include <rime/dict/rewrite_stage.h>

namespace rime {

struct RewriteStageId {
  uint64_t high = 0;
  uint64_t low = 0;

  bool empty() const { return high == 0 && low == 0; }
  bool operator==(const RewriteStageId& other) const {
    return high == other.high && low == other.low;
  }
};

// One immutable named resource; the index contains only an ordered stage
// sequence, never schema ids or filter instance names.
class RIME_DLL RewriteStore {
 public:
  static string FileName(const string& name);
  static an<const RewriteStore> Open(const path& file_path);
  static bool Build(const path& file_path,
                    const vector<RewriteStageId>& ids,
                    const function<bool(size_t, RewriteStageData*)>& load,
                    bool force = false,
                    const vector<path>& reusable = {});

  bool Lookup(std::string_view text, vector<string>* values) const;
  bool LookupWithPreedit(std::string_view text,
                         vector<RewriteResult>* values) const;
  bool ConvertSentence(std::string_view text,
                       string* output,
                       vector<RewriteSentenceState>* states = nullptr) const;

  size_t stage_count() const { return stages_.size(); }
  uint64_t generation() const { return generation_; }

 private:
  vector<an<const RewriteStage>> stages_;
  uint64_t generation_ = 0;
};

// Shared lazy resource slot. Active translations pin an immutable snapshot;
// deployment clears slots before replacing files, without knowing any gear.
class RIME_DLL RewriteResource {
 public:
  static an<RewriteResource> Require(const string& name);
  static void ReleaseForDeployment();
  an<const RewriteStore> Acquire();

 private:
  RewriteResource(path staging, path prebuilt, string file_name);
  path staging_;
  path prebuilt_;
  string file_name_;
  std::mutex mutex_;
  bool attempted_ = false;
  an<const RewriteStore> store_;
};

}  // namespace rime

#endif  // RIME_REWRITE_STORE_H_
