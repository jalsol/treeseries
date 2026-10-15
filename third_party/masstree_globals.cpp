// masstree declares its mrcu/epoch globals as extern; the TU that defines
// GLOBAL_VALUE_DEFINE emits them. Exactly one TU may do this.
#define GLOBAL_VALUE_DEFINE
#include "third_party/masstree_wrapper.hpp"
