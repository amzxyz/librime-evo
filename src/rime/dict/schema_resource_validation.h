// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_SCHEMA_RESOURCE_VALIDATION_H_
#define RIME_SCHEMA_RESOURCE_VALIDATION_H_

#include <rime/common.h>

namespace rime {
class Config;
RIME_DLL bool ValidateSchemaResources(Config* config);
}  // namespace rime

#endif  // RIME_SCHEMA_RESOURCE_VALIDATION_H_
