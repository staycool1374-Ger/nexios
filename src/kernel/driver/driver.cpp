/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file driver.cpp
/// @brief Implementation of the driver registry.

#include <kernel/driver/driver.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>

namespace kernel {

constinit Driver *DriverRegistry::drivers_[MAX_DRIVERS] = {};
constinit size_t DriverRegistry::count_ = 0;

void DriverRegistry::init() {
    count_ = 0;
}

void DriverRegistry::register_driver(const char *name, const char *description,
                                     bool (*init)(), void (*exit)(),
                                     uint32_t irq_line) {
    if (count_ >= MAX_DRIVERS)
        return;
    auto *drv = static_cast<Driver *>(MemPool::alloc(sizeof(Driver)));
    if (!drv)
        return;
    *drv = {name, description, init, exit, DriverState::UNLOADED, irq_line};
    kernel::test::ResourceTracker::instance().track_driver_add();
    drivers_[count_++] = drv;
}

bool DriverRegistry::load(const char *name) {
    for (size_t i = 0; i < count_; ++i) {
        auto *drv = drivers_[i];
        if (!drv || !drv->name)
            continue;
        const char *a = drv->name;
        const char *b = name;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*a || *b)
            continue;

        if (drv->state == DriverState::LOADED)
            return true;
        if (!drv->init)
            return false;
        bool ok = drv->init();
        drv->state = ok ? DriverState::LOADED : DriverState::ERROR;
        return ok;
    }
    return false;
}

void DriverRegistry::unload(const char *name) {
    for (size_t i = 0; i < count_; ++i) {
        auto *drv = drivers_[i];
        if (!drv || !drv->name)
            continue;
        const char *a = drv->name;
        const char *b = name;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*a || *b)
            continue;

        if (drv->state == DriverState::LOADED && drv->exit) {
            drv->exit();
        }
        drv->state = DriverState::UNLOADED;
        kernel::test::ResourceTracker::instance().track_driver_remove();
        MemPool::free(drv);
        drivers_[i] = nullptr;
        return;
    }
}

void DriverRegistry::unload_all() {
    for (size_t i = 0; i < count_; ++i) {
        auto *drv = drivers_[i];
        if (!drv)
            continue;

        if (drv->state == DriverState::LOADED && drv->exit) {
            drv->exit();
        }
        drv->state = DriverState::UNLOADED;
        kernel::test::ResourceTracker::instance().track_driver_remove();
        MemPool::free(drv);
        drivers_[i] = nullptr;
    }
    count_ = 0;
}

const Driver *DriverRegistry::find(const char *name) {
    for (size_t i = 0; i < count_; ++i) {
        auto *drv = drivers_[i];
        if (!drv || !drv->name)
            continue;
        const char *a = drv->name;
        const char *b = name;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0')
            return drv;
    }
    return nullptr;
}

} // namespace kernel
