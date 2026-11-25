//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_REWRITE_COMPILER_H_
#define RIME_REWRITE_COMPILER_H_

#include <rime_api.h>
#include <rime/common.h>
#include <rime/dict/rewrite_store.h>
#include <rime/lever/build_compiler.h>

namespace rime {

class Config;
class Deployer;

class RIME_DLL RewriteCompiler : public BuildCompiler {
 public:
  struct SourceFile {
    string declared_name;
    path resolved_path;
  };
  struct Stage {
    string name;
    string key_xlit;
    vector<SourceFile> files;
  };
  struct Spec {
    string name;
    vector<Stage> stages;
    vector<RewriteStageId> ids;
    vector<string> aliases;
    uint64_t estimated_memory = 0;
  };

  explicit RewriteCompiler(Deployer* deployer) : deployer_(deployer) {}
  bool ReadSpecs(Config* config, vector<Spec>* specs);
  static bool Compile(const Spec& spec,
                      Deployer* deployer,
                      bool rebuild = false);
  bool Collect(const BuildRequest& request, BuildPlan* plan) override;
  bool Prepare() override;
  void Finish() override;

 private:
  struct CachedStage {
    string key_xlit;
    vector<path> files;
    RewriteStageId id;
  };
  Deployer* deployer_;
  vector<CachedStage> stage_cache_;
  struct Build {
    an<Spec> spec;
    size_t unit;
    string schema;
  };
  map<string, Build> builds_;
};

}  // namespace rime

#endif  // RIME_REWRITE_COMPILER_H_
