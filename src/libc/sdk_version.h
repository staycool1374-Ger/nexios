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

#pragma once

// SDK internal versioning from 0.1 (spec §14, issue #171 P0): SDK layout,
// Capability-API, and crt0 entry contract version independently. SDK 0.x =
// unstable: layout/contract may break on minor, API calls append-only.
// CRT0_CONTRACT starts at 1 (0 is reserved = unset, same doctrine as
// tls_base_ == 0); enforced by the crt0 self-check matrix (§14), the
// loader stays dumb. Freestanding-clean: no includes, so kernel test TUs
// and user programs can consume these macros without pulling libc types.
#define NEXIOS_SDK_MAJOR     0
#define NEXIOS_SDK_MINOR     1
#define NEXIOS_CAP_API_MAJOR 0
#define NEXIOS_CAP_API_MINOR 1
#define NEXIOS_CRT0_CONTRACT 1
