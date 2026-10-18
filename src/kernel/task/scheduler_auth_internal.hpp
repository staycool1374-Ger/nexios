#pragma once

/// @file scheduler_auth_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_auth.cpp
///        (issue #323).  Pure move: no logic change.
///        The user-ELF authenticity slice owns no shared cross-TU state:
///        kAuthSlicePages stays file-local in scheduler_auth.cpp and no
///        staying-TU symbol is defined here.  This header exists only to
///        keep the per-strand convention uniform (one internal header per
///        extracted TU).

#include <kernel/task/scheduler.hpp>

namespace kernel::sched_auth {

// No shared cross-TU state.  kAuthSlicePages stays file-local in
// scheduler_auth.cpp; auth_verify_step is declared in scheduler.hpp
// and defined once in scheduler_auth.cpp.

}  // namespace kernel::sched_auth
