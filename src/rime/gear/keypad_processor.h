//
// Copyright RIME Developers
// Distributed under the BSD License
//

#ifndef RIME_KEYPAD_PROCESSOR_H_
#define RIME_KEYPAD_PROCESSOR_H_

#include <rime/processor.h>

namespace rime {

// Routes physical numeric keypad digits either to the input context or to the
// selector processor that follows it in the processor chain. Main-row digits
// are intentionally left for selector.
class KeypadProcessor : public Processor {
 public:
  explicit KeypadProcessor(const Ticket& ticket);

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  enum Mode {
    kAuto,
    kCompose,
    kSelect,
  };

  static const char* ModeName(Mode mode);

  Mode mode_ = kSelect;
  bool raw_mode_ = false;
};

}  // namespace rime

#endif  // RIME_KEYPAD_PROCESSOR_H_
