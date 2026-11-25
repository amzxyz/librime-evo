//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/lever/dictionary_build_compiler.h>

#include <boost/algorithm/string/predicate.hpp>
#include <rime/config.h>
#include <rime/path_resolver.h>

namespace rime {
namespace {

bool SamePrismAlgebra(const an<ConfigList>& first,
                      const an<ConfigList>& second) {
  const size_t size = first ? first->size() : 0;
  if (size != (second ? second->size() : 0))
    return false;
  for (size_t i = 0; i < size; ++i) {
    auto a = As<ConfigValue>(first->GetAt(i));
    auto b = As<ConfigValue>(second->GetAt(i));
    if (!a || !b || a->str() != b->str())
      return false;
  }
  return true;
}

}  // namespace

bool DictionaryBuildCompiler::Collect(const BuildRequest& request,
                                      BuildPlan* plan) {
  vector<DictCompiler::DictionarySpec> definitions;
  if (!DictCompiler::ReadDictionarySpecs(request.config, &definitions))
    return false;
  const int options = (request.rebuild ? DictCompiler::kRebuild : 0) |
                      (request.dump ? DictCompiler::kDump : 0);
  const auto& schema_id = request.schema_id;
  const auto& schema_file = request.schema_file;
  for (const auto& definition : definitions) {
    auto existing = dictionaries_.find(definition.name);
    if (existing == dictionaries_.end()) {
      auto shared = New<DictCompiler::DictionarySpec>();
      shared->name = definition.name;
      shared->packs = definition.packs;
      CompileUnit unit("dictionary:" + definition.name,
                       DictCompiler::EstimateMemory(definition.name, false,
                                                    definition.packs),
                       [shared, schema_file, options]() {
                         return DictCompiler::CompileDictionary(
                             *shared, schema_file, options);
                       });
      for (const char* suffix :
           {".table.bin", ".reverse.bin", ".packs.table.bin"}) {
        unit.AddOutput(
            path(definition.name + suffix).filename().to_utf8_string());
      }
      const size_t index = plan->AddUnit(std::move(unit), schema_id);
      existing = dictionaries_
                     .emplace(definition.name,
                              DictionaryBuild{shared, index, schema_id})
                     .first;
    } else if (existing->second.definition->packs != definition.packs) {
      LOG(ERROR) << "conflicting packs for dictionary '" << definition.name
                 << "' in schemas '" << existing->second.schema << "' and '"
                 << schema_id << "'";
      return false;
    }
    auto& build = existing->second;
    plan->AddOwner(build.unit, schema_id);
    auto& unit = plan->unit(build.unit);
    for (const auto& prism : definition.prisms) {
      const string output =
          path(prism.first + ".prism.bin").filename().to_utf8_string();
      const auto [owner, added] = prisms_.emplace(
          output, PrismBuild{definition.name, schema_id, prism.second});
      if (added) {
        build.definition->prisms.push_back(prism);
        unit.AddOutput(output);
      } else if (owner->second.dictionary != definition.name ||
                 !SamePrismAlgebra(owner->second.algebra, prism.second)) {
        LOG(ERROR) << "conflicting prism '" << output << "' in schemas '"
                   << owner->second.schema << "' and '" << schema_id
                   << "'; use distinct algebra group names";
        return false;
      }
    }
  }
  return true;
}

bool AuxiliaryBuildCompiler::Collect(const BuildRequest& request,
                                     BuildPlan* plan) {
  Groups groups;
  if (!DictCompiler::ReadAuxiliarySpecs(request.config, &groups))
    return false;
  if (groups.empty())
    return true;
  string source;
  if (!request.config->GetString("build/auxiliary/dictionary", &source) ||
      source.empty()) {
    LOG(ERROR) << "build/auxiliary/dictionary is required.";
    return false;
  }
  if (!boost::iends_with(source, ".yaml"))
    source += ".yaml";
  for (const auto& group : groups) {
    if (!safe_path::ValidRelativeName(group.first, false))
      return false;
    const auto existing = groups_.find(group.first);
    if (existing != groups_.end()) {
      const auto& build = existing->second;
      if (build.source != source ||
          !SamePrismAlgebra(build.algebra, group.second)) {
        LOG(ERROR) << "conflicting auxiliary group '" << group.first
                   << "' in schemas '" << build.schema << "' and '"
                   << request.schema_id << "'.";
        return false;
      }
      plan->AddOwner(build.unit, request.schema_id);
      continue;
    }
    const string id = "auxiliary:" + group.first;
    auto source_build = sources_.find(source);
    if (source_build == sources_.end()) {
      auto shared = New<Groups>();
      const auto name = source.substr(0, source.size() - 5);
      const auto bytes = DictCompiler::EstimateMemory(name, true);
      const bool rebuild = request.rebuild;
      CompileUnit unit(id, bytes, [source, shared, rebuild]() {
        return DictCompiler::CompileAuxiliary(source, *shared, rebuild);
      });
      const size_t index = plan->AddUnit(std::move(unit), request.schema_id);
      source_build = sources_.emplace(source, SourceBuild{shared, index}).first;
    }
    auto& build = source_build->second;
    if (!plan->AddAlias(id, build.unit))
      return false;
    build.groups->push_back(group);
    plan->unit(build.unit).AddOutput(group.first + ".aux.bin");
    plan->AddOwner(build.unit, request.schema_id);
    GroupBuild definition{source, request.schema_id, group.second, build.unit};
    groups_.emplace(group.first, std::move(definition));
  }
  return true;
}

}  // namespace rime
