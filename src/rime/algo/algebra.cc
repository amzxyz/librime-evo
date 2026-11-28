//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-01-19 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <fstream>
#include <rime/algo/algebra.h>
#include <rime/algo/calculus.h>

namespace rime {

bool Script::AddSyllable(const string& syllable) {
  if (find(syllable) != end())
    return false;
  Spelling spelling(syllable);
  (*this)[syllable].push_back(spelling);
  return true;
}

namespace {
bool Dominates(const SpellingProperties& a, const SpellingProperties& b) {
  return (a.fuzzy_mask & b.fuzzy_mask) == a.fuzzy_mask && a.type <= b.type &&
         a.credibility >= b.credibility &&
         (!a.is_correction || b.is_correction) && a.tips == b.tips &&
         a.auxiliary_display == b.auxiliary_display;
}
}  // namespace

void Script::Merge(const string& s,
                   const SpellingProperties& sp,
                   const vector<Spelling>& v,
                   bool conditional) {
  vector<Spelling>& m((*this)[s]);
  for (const Spelling& x : v) {
    Spelling y(x);
    y.properties.Compose(sp);
    auto same_source = [&](const Spelling& z) {
      return z.str == y.str && z.properties.auxiliary == y.properties.auxiliary;
    };
    auto e = std::find_if(m.begin(), m.end(), [&](const Spelling& z) {
      return same_source(z) &&
             z.properties.fuzzy_mask == y.properties.fuzzy_mask;
    });
    if (!conditional) {
      if (e == m.end())
        m.push_back(std::move(y));
      else
        e->properties.Update(y.properties);
      continue;
    }
    if (e != m.end()) {
      e->properties.Update(y.properties);
      y = *e;
    }
    // Independent masks are alternative paths. Drop only dominated paths.
    auto dominates = [&](const Spelling& z) {
      return same_source(z) &&
             z.properties.fuzzy_mask != y.properties.fuzzy_mask &&
             Dominates(z.properties, y.properties);
    };
    if (std::any_of(m.begin(), m.end(), dominates)) {
      if (e != m.end())
        m.erase(e);
      continue;
    }
    if (e == m.end())
      m.push_back(y);
    auto dominated = [&](const Spelling& z) {
      return same_source(z) &&
             z.properties.fuzzy_mask != y.properties.fuzzy_mask &&
             Dominates(y.properties, z.properties);
    };
    m.erase(std::remove_if(m.begin(), m.end(), dominated), m.end());
  }
}

void Script::Dump(const path& file_path) const {
  std::ofstream out(file_path.c_str());
  for (const value_type& v : *this) {
    bool first = true;
    for (const Spelling& s : v.second) {
      out << (first ? v.first : "") << '\t' << s.str << '\t'
          << "-ac?!"[s.properties.type] << '\t' << s.properties.credibility
          << '\t' << s.properties.tips << '\t' << s.properties.fuzzy_mask
          << std::endl;
      first = false;
    }
  }
  out.close();
}

bool Projection::Load(an<ConfigList> settings, bool allow_fuzzy) {
  if (!settings)
    return false;
  calculation_.clear();
  fuzzy_groups_.clear();
  Calculus calc;
  bool success = true;
  for (size_t i = 0; i < settings->size(); ++i) {
    an<ConfigValue> v(settings->GetValueAt(i));
    if (!v) {
      LOG(ERROR) << "Error loading formula #" << (i + 1) << ".";
      success = false;
      break;
    }
    const string& formula(v->str());
    an<Calculation> x;
    try {
      x.reset(calc.Parse(formula));
    } catch (boost::regex_error& e) {
      LOG(ERROR) << "Error parsing formula '" << formula << "': " << e.what();
    }
    if (!x || (!allow_fuzzy && dynamic_cast<FuzzyDerivation*>(x.get()))) {
      LOG(ERROR) << "Error loading spelling algebra definition #" << (i + 1)
                 << ": '" << formula << "'.";
      success = false;
      break;
    }
    calculation_.push_back(x);
  }
  if (!success) {
    calculation_.clear();
  } else {
    fuzzy_groups_ = calc.fuzzy_groups();
  }
  return success;
}

bool Projection::Apply(string* value) {
  if (!value || value->empty() || !fuzzy_groups_.empty())
    return false;
  bool modified = false;
  Spelling s(*value);
  for (an<Calculation>& x : calculation_) {
    try {
      if (x->Apply(&s))
        modified = true;
    } catch (std::runtime_error& e) {
      LOG(ERROR) << "Error applying calculation: " << e.what();
      return false;
    }
  }
  if (modified)
    value->assign(s.str);
  return modified;
}

bool Projection::Apply(Script* value) {
  return Apply(value, nullptr);
}

bool Projection::Apply(Script* value, set<string>* display_codes) {
  if (!value || value->empty())
    return false;
  bool modified = false;
  const bool conditional = !fuzzy_groups_.empty();
  int round = 0;
  for (an<Calculation>& x : calculation_) {
    ++round;
    DLOG(INFO) << "round #" << round;
    Script temp;
    for (const Script::value_type& v : *value) {
      Spelling s(v.first);
      bool applied = false;
      try {
        applied = x->Apply(&s);
      } catch (std::runtime_error& e) {
        LOG(ERROR) << "Error applying calculation: " << e.what();
        return false;
      }
      if (applied) {
        modified = true;
        if (!x->deletion()) {
          temp.Merge(v.first, SpellingProperties(), v.second, conditional);
        }
        if (x->addition() && !s.str.empty()) {
          // Record only the explicitly marked spelling, before further rules
          // can derive other variants from it.
          if (display_codes && s.properties.auxiliary_display)
            display_codes->insert(s.str);
          temp.Merge(s.str, s.properties, v.second, conditional);
        }
      } else {
        temp.Merge(v.first, SpellingProperties(), v.second, conditional);
      }
    }
    value->swap(temp);
  }
  return modified;
}

}  // namespace rime
