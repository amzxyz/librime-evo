//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_AUXILIARY_SUFFIX_SEGMENTOR_H_
#define RIME_AUXILIARY_SUFFIX_SEGMENTOR_H_

#include <rime/segmentor.h>

namespace rime {

// Applies a trailing `AUX suffix to the ordinary spelling segment.
class AuxiliarySuffixSegmentor : public Segmentor {
 public:
  explicit AuxiliarySuffixSegmentor(const Ticket& ticket);

  bool Proceed(Segmentation* segmentation) override;

 private:
  string trigger_ = "`";
};

}  // namespace rime

#endif  // RIME_AUXILIARY_SUFFIX_SEGMENTOR_H_
