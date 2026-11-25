//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/lever/build_compiler.h>

#include <rime/config.h>
#include <rime/setup.h>

namespace rime {

BuildPlan::BuildPlan(Deployer* deployer, bool rebuild, bool dump)
    : deployer_(deployer), rebuild_(rebuild), dump_(dump) {
  // C++ callers can invoke deployment tasks directly after normal runtime
  // initialization, which does not load the levers module.
  LoadModules(kDeployerModules);
}

bool BuildPlan::Collect(const string& schema_id,
                        Config* config,
                        const path& schema_file) {
  if (!valid_ || started_ || !deployer_ || !config) {
    LOG(ERROR) << "cannot collect build plan for schema '" << schema_id
               << "': invalid plan or configuration.";
    valid_ = false;
    return false;
  }
  if (config->IsNull("build"))
    return true;
  auto build = config->GetMap("build");
  if (!build) {
    LOG(ERROR) << "build must be a map of resource definitions.";
    valid_ = false;
    return false;
  }
  const BuildRequest request{schema_id, config, schema_file, rebuild_, dump_};
  for (const auto& entry : *build) {
    const string key = "build/" + entry.first;
    auto it = compilers_.find(key);
    if (it == compilers_.end()) {
      auto* component = BuildCompiler::Require(key);
      if (!component) {
        LOG(ERROR) << "schema '" << schema_id
                   << "': no compiler registered for '" << key << "'.";
        valid_ = false;
        return false;
      }
      the<BuildCompiler> compiler(component->Create(deployer_));
      if (!compiler) {
        LOG(ERROR) << "schema '" << schema_id
                   << "': cannot create compiler '" << key << "'.";
        valid_ = false;
        return false;
      }
      it = compilers_.emplace(key, std::move(compiler)).first;
    }
    if (!it->second->Collect(request, this)) {
      LOG(ERROR) << "invalid '" << key << "' in schema '" << schema_id << "'.";
      valid_ = false;
      return false;
    }
  }
  return true;
}

size_t BuildPlan::AddUnit(CompileUnit unit, const string& owner) {
  const size_t index = units_.size();
  owners_[unit.id()].insert(owner);
  units_.push_back(std::move(unit));
  AddAlias(units_[index].id(), index);
  return index;
}

void BuildPlan::AddOwner(size_t index, const string& owner) {
  owners_[units_[index].id()].insert(owner);
}

bool BuildPlan::AddAlias(const string& id, size_t index) {
  if (id.empty() || index >= units_.size() || started_) {
    valid_ = false;
    return false;
  }
  const auto [it, added] = unit_ids_.emplace(id, index);
  if (!added && it->second != index) {
    LOG(ERROR) << "conflicting compile unit id '" << id << "'.";
    valid_ = false;
    return false;
  }
  return true;
}

void BuildPlan::AddDependency(const string& prerequisite,
                              const string& dependent) {
  dependencies_.emplace_back(prerequisite, dependent);
}

bool BuildPlan::Run(set<string>* failed_schemas) {
  if (!valid_ || started_ || !failed_schemas) {
    LOG(ERROR) << "cannot run invalid or already started build plan.";
    return false;
  }
  started_ = true;
  failed_schemas->clear();
  vector<pair<string, string>> resolved;
  for (const auto& dependency : dependencies_) {
    auto from = unit_ids_.find(dependency.first);
    auto to = unit_ids_.find(dependency.second);
    if (from == unit_ids_.end() || to == unit_ids_.end()) {
      LOG(ERROR) << "unknown compile dependency '" << dependency.first
                 << "' -> '" << dependency.second << "'.";
      return false;
    }
    if (dependency.first != dependency.second && from->second == to->second)
      continue;  // Both resources are produced by one unit.
    resolved.emplace_back(units_[from->second].id(), units_[to->second].id());
  }
  CompileGraph graph;
  for (auto& unit : units_) {
    if (!graph.AddUnit(std::move(unit)))
      return false;
  }
  for (const auto& dependency : resolved) {
    if (!graph.AddDependency(dependency.first, dependency.second))
      return false;
  }

  struct FinishCompilers {
    vector<BuildCompiler*> prepared;
    ~FinishCompilers() { Finish(); }

    bool Finish() {
      bool success = true;
      while (!prepared.empty()) {
        auto* compiler = prepared.back();
        prepared.pop_back();
        try {
          compiler->Finish();
        } catch (const std::exception& ex) {
          LOG(ERROR) << "compiler cleanup failed: " << ex.what();
          success = false;
        } catch (...) {
          LOG(ERROR) << "compiler cleanup failed.";
          success = false;
        }
      }
      return success;
    }
  } finish;
  for (const auto& entry : compilers_) {
    finish.prepared.push_back(entry.second.get());
    if (!entry.second->Prepare()) {
      LOG(ERROR) << "compiler preparation failed: " << entry.first;
      return false;
    }
  }
  CompileScheduler scheduler;
  auto result = scheduler.Run(graph);
  if (!result) {
    LOG(ERROR) << "compile scheduler failed to execute build plan.";
    return false;
  }
  auto report_failure = [&](const string& unit, const char* reason) {
    const auto& owners = owners_.at(unit);
    for (const auto& schema : owners) {
      LOG(ERROR) << "schema '" << schema << "': compile unit '" << unit << "' "
                 << reason << ".";
      failed_schemas->insert(schema);
    }
  };
  for (const auto& unit : result->failed_units)
    report_failure(unit, "failed");
  for (const auto& unit : result->skipped_units)
    report_failure(unit, "skipped because a prerequisite failed");
  return finish.Finish();
}

}  // namespace rime
