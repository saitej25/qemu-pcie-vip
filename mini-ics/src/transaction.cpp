// Most of the transaction/queue machinery in transaction.hpp is
// templated or trivially inline and has no out-of-line body. This
// translation unit exists for the non-template helpers that benefit
// from being compiled once rather than duplicated per include.
#include "mini_ics/transaction.hpp"

namespace mini_ics {

// Out-of-line to give the vtable-free class a stable key function and
// avoid duplicate-symbol / -Wweak-vtables style warnings across
// translation units that include transaction.hpp.
PendingRequestTable::~PendingRequestTable() = default;

}  // namespace mini_ics
