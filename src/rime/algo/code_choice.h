// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_CODE_CHOICE_H_
#define RIME_CODE_CHOICE_H_

#include <rime/common.h>

namespace rime {

struct CodeChoice {
  size_t start;
  size_t end;
  string label;
  string replacement;
};

}  // namespace rime

#endif  // RIME_CODE_CHOICE_H_
