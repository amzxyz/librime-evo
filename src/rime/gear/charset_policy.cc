// Copyright RIME Developers
// Distributed under the BSD License
#include <rime/gear/charset_policy.h>

#include <utf8.h>
#include <rime/config.h>
#include <rime/config/resource_config.h>
#include <rime/context.h>
#include <rime/dict/rewrite_store.h>
#include <rime/segmentation.h>

namespace rime {
namespace {

using Mask = std::bitset<256>;

Mask Attributes(const string& text) {
  Mask mask;
  for (unsigned char c : text)
    mask.set(c);
  return mask;
}

void ReadStrings(const an<ConfigItem>& item, vector<string>* result) {
  if (auto value = As<ConfigValue>(item)) {
    if (!value->str().empty())
      result->push_back(value->str());
  } else if (auto list = As<ConfigList>(item)) {
    for (auto it = list->begin(); it != list->end(); ++it)
      if (auto value = As<ConfigValue>(*it))
        if (!value->str().empty())
          result->push_back(value->str());
  }
}

bool ReadCharacters(const an<ConfigItem>& item, hash_set<uint32_t>* result) {
  vector<string> strings;
  ReadStrings(item, &strings);
  for (const auto& text : strings) {
    if (!utf8::is_valid(text.begin(), text.end()))
      return false;
    for (auto p = text.begin(); p != text.end();)
      result->insert(utf8::next(p, text.end()));
  }
  return true;
}

bool IsHan(uint32_t ch) {
  return (ch >= 0x4e00 && ch <= 0x9fff) || (ch >= 0x3400 && ch <= 0x4dbf) ||
         (ch >= 0x20000 && ch <= 0x2a6df) || (ch >= 0x2a700 && ch <= 0x2b73f) ||
         (ch >= 0x2b740 && ch <= 0x2b81f) || (ch >= 0x2b820 && ch <= 0x2ceaf) ||
         (ch >= 0x2ceb0 && ch <= 0x2ebef) || (ch >= 0x2ebf0 && ch <= 0x2ee5f) ||
         (ch >= 0x30000 && ch <= 0x3347f) || (ch >= 0xf900 && ch <= 0xfaff) ||
         (ch >= 0x2f800 && ch <= 0x2fa1f) || (ch >= 0x2e80 && ch <= 0x2eff) ||
         (ch >= 0x2f00 && ch <= 0x2fdf);
}

struct Checker {
  an<const RewriteStore> store;
  Mask base;
  hash_set<uint32_t> add;
  hash_set<uint32_t> ban;
  hash_map<uint32_t, bool> cache;

  bool Accept(const string& text) {
    // Existing multi-character words are not decomposed or checked.
    if (text.size() < 3 || text.size() > 4 ||
        !utf8::is_valid(text.begin(), text.end()))
      return true;
    auto p = text.begin();
    const auto ch = utf8::next(p, text.end());
    if (p != text.end() || !IsHan(ch))
      return true;
    auto found = cache.find(ch);
    if (found != cache.end())
      return found->second;
    bool allowed = false;
    if (!ban.count(ch)) {
      allowed = add.count(ch) != 0;
      if (!allowed && base.any()) {
        string value;
        allowed = store->LookupFirst(text, &value) &&
                  (Attributes(value) & base).any();
      }
    }
    cache.emplace(ch, allowed);
    return allowed;
  }
};

}  // namespace

an<CharsetPolicy> CharsetPolicy::Load(Config* config,
                                      const string& name_space) {
  string data;
  if (!config || !config->GetString(name_space + "/data", &data) ||
      !ResolveResourceReference(config, "rewrite", &data))
    return nullptr;
  auto policy = New<CharsetPolicy>();
  policy->resource_ = RewriteResource::Require(data);
  auto rules = config->GetList(name_space + "/rules");
  if (!policy->resource_ || !rules) {
    LOG(ERROR) << "invalid charset policy: " << name_space;
    return nullptr;
  }
  ReadStrings(config->GetItem(name_space + "/exclude_tags"),
              &policy->exclude_tags_);
  for (auto it = rules->begin(); it != rules->end(); ++it) {
    auto map = As<ConfigMap>(*it);
    if (!map)
      continue;
    Rule rule;
    ReadStrings(map->Get("option"), &rule.options);
    ReadStrings(map->Get("options"), &rule.options);
    if (rule.options.empty())
      continue;
    if (auto value = map->GetValue("base"))
      rule.base = Attributes(value->str());
    if (!ReadCharacters(map->Get("addlist"), &rule.add) ||
        !ReadCharacters(map->Get("blacklist"), &rule.ban)) {
      LOG(ERROR) << "invalid charset character list: " << name_space;
      return nullptr;
    }
    policy->rules_.push_back(std::move(rule));
  }
  return policy;
}

DictTextFilter CharsetPolicy::Bind(const Context* context,
                                   const Segment* segment) const {
  if (!context || (segment && segment->HasAnyTagIn(exclude_tags_)))
    return {};
  an<Checker> checker;
  for (const auto& rule : rules_) {
    bool active = false;
    for (const auto& option : rule.options) {
      if (option == "true" ||
          (option != "false" && context->get_option(option))) {
        active = true;
        break;
      }
    }
    if (!active)
      continue;
    if (!checker)
      checker = New<Checker>();
    checker->base |= rule.base;
    checker->add.insert(rule.add.begin(), rule.add.end());
    checker->ban.insert(rule.ban.begin(), rule.ban.end());
  }
  if (!checker)
    return {};
  checker->store = resource_->Acquire();
  // An unavailable database must never turn a scheme into an empty menu.
  if (!checker->store)
    return {};
  return [checker](const string& text) { return checker->Accept(text); };
}

}  // namespace rime
