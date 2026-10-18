#pragma once

/// @file scheduler_registry_internal.hpp
/// @brief Shared cross-TU declarations for scheduler_registry.cpp (issue
///        #323).  Pure move support: no logic changes.
///        The task-registry engine (task-set administration, affinity,
///        balancer, static task-table homes) needs no cross-TU shared home:
///        all moved functions are Scheduler:: members (declared in
///        scheduler.hpp) and all moved statics are single-homed member
///        definitions.  This header exists for strand-pattern consistency
///        and future shared decls.

#include <kernel/task/scheduler.hpp>
