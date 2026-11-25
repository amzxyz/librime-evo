//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_DICTIONARY_BUILD_COMPILER_H_
#define RIME_DICTIONARY_BUILD_COMPILER_H_

#include <rime/dict/dict_compiler.h>
#include <rime/lever/build_compiler.h>

namespace rime {

class DictionaryBuildCompiler : public BuildCompiler {
 public:
  explicit DictionaryBuildCompiler(Deployer*) {}
  bool Collect(const BuildRequest& request, BuildPlan* plan) override;

 private:
  struct DictionaryBuild {
    an<DictCompiler::DictionarySpec> definition;
    size_t unit;
    string schema;
  };
  struct PrismBuild {
    string dictionary;
    string schema;
    an<ConfigList> algebra;
  };
  map<string, DictionaryBuild> dictionaries_;
  map<string, PrismBuild> prisms_;
};

class AuxiliaryBuildCompiler : public BuildCompiler {
 public:
  explicit AuxiliaryBuildCompiler(Deployer*) {}
  bool Collect(const BuildRequest& request, BuildPlan* plan) override;

 private:
  using Groups = vector<pair<string, an<ConfigList>>>;
  struct SourceBuild {
    an<Groups> groups;
    size_t unit;
  };
  struct GroupBuild {
    string source;
    string schema;
    an<ConfigList> algebra;
    size_t unit;
  };
  map<string, SourceBuild> sources_;
  map<string, GroupBuild> groups_;
};

}  // namespace rime

#endif  // RIME_DICTIONARY_BUILD_COMPILER_H_
