#pragma once

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

/// @file driver.hpp
/// @brief Driver registry and module management layer.

#include <types.hpp>

namespace kernel {

/// @brief Describes the current state of a driver module.
enum class DriverState : uint8_t {
    UNLOADED,
    LOADED,
    ERROR,
};

/// @brief Describes a single driver module.
struct Driver {
    const char *name;
    const char *description;
    bool (*init)();
    void (*exit)();
    DriverState state;
    uint32_t irq_line;

    Driver()
        : name(nullptr), description(nullptr), init(nullptr), exit(nullptr),
          state(DriverState::UNLOADED), irq_line(0) {
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    Driver(const char *name_, const char *description_, bool (*init_)(),
           void (*exit_)(), DriverState state_, uint32_t irq_line_)
        : name(name_), description(description_), init(init_), exit(exit_),
          state(state_), irq_line(irq_line_) {
    }
};

/// @brief Central registry for kernel driver modules.
class DriverRegistry {
  public:
    /// @brief Initialises the driver registry.
    static void init();

    /// @brief Registers a driver module.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    static void register_driver(const char *name, const char *description,
                                bool (*init)(), void (*exit)(),
                                uint32_t irq_line = 0);

    /// @brief Loads and initialises a driver by name.
    /// @return true on success, false if not found or init failed.
    static bool load(const char *name);

    /// @brief Unloads a driver by name.
    static void unload(const char *name);

    /// @brief Unloads all registered drivers.
    static void unload_all();

    /// @brief Looks up a driver by name.
    /// @return Pointer to the Driver, or nullptr.
    static const Driver *find(const char *name);

    /// @brief Returns the number of registered drivers.
    static size_t count() {
        return count_;
    }

    /// @brief Returns the driver at the given index.
    static const Driver *get(size_t index) {
        return index < count_ ? drivers_[index] : nullptr;
    }

  private:
    static constexpr size_t MAX_DRIVERS = CONFIG_MAX_DRIVERS;
    static constinit Driver *drivers_[MAX_DRIVERS];
    static constinit size_t count_;
};

} // namespace kernel
