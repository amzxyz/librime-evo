//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_BUILD_COMPILER_H_
#define RIME_BUILD_COMPILER_H_

#include <rime_api.h>
#include <rime/common.h>
#include <rime/component.h>
#include <rime/lever/compile_unit.h>

namespace rime {

class Config;
class Deployer;
class BuildPlan;

struct BuildRequest {
  const string& schema_id;
  Config* config;
  const path& schema_file;
  bool rebuild;
  bool dump;
};

// Registered as "build/<section>". One instance collects all definitions of
// its resource type for one deployment, and declares work to the scheduler.
class RIME_DLL BuildCompiler : public Class<BuildCompiler, Deployer*> {
 public:
  virtual ~BuildCompiler() = default;
  // The request is borrowed for this call only; copy data needed by a unit.
  virtual bool Collect(const BuildRequest& request, BuildPlan* plan) = 0;
  virtual bool Prepare() { return true; }
  virtual void Finish() {}
};

class RIME_DLL BuildPlan {
 public:
  explicit BuildPlan(Deployer* deployer,
                     bool rebuild = false,
                     bool dump = false);
  BuildPlan(const BuildPlan&) = delete;
  BuildPlan& operator=(const BuildPlan&) = delete;

  bool Collect(const string& schema_id,
               Config* config,
               const path& schema_file);
  size_t AddUnit(CompileUnit unit, const string& owner);
  void AddOwner(size_t index, const string& owner);
  // Additional resource names may refer to the same compile unit.
  bool AddAlias(const string& id, size_t index);
  CompileUnit& unit(size_t index) { return units_[index]; }
  // Unit IDs are resolved after all schemas have been collected.
  // A failed prerequisite prevents its dependent from running.
  void AddDependency(const string& prerequisite, const string& dependent);
  // Planning errors abort before any unit runs; unit failures are reported
  // against all schemas that reference the corresponding resource.
  bool Run(set<string>* failed_schemas);

 private:
  Deployer* deployer_;
  bool rebuild_;
  bool dump_;
  bool valid_ = true;
  bool started_ = false;
  map<string, the<BuildCompiler>> compilers_;
  vector<CompileUnit> units_;
  map<string, size_t> unit_ids_;
  map<string, set<string>> owners_;
  vector<pair<string, string>> dependencies_;
};

}  // namespace rime

#endif  // RIME_BUILD_COMPILER_H_
