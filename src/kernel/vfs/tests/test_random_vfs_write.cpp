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

/// @file test_random_vfs_write.cpp
/// @brief Random VFS write operation tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/vfs.hpp>

using namespace kernel;
using namespace kernel::vfs;

// Runmode: kernel
// Testidea: Writing to /dev/random via vnode ops returns the byte count
// Input: vnode->ops->write on /dev/random vnode with 128-byte buffer
// Expect: Returns 128 (write is a no-op that echoes count)
// Depends: kernel::vfs::resolve, VnodeOps::write
JARVIS_TEST(dev_random_write, "PRE: vfsd, iocd | POST: none") {
    Vnode *vn = vfs::resolve("/dev/random");
    JARVIS_ASSERT(vn != nullptr);
    JARVIS_ASSERT(vn->ops != nullptr);
    JARVIS_ASSERT(vn->ops->write != nullptr);

    uint8_t buf[128] = {42};
    int64_t written = vn->ops->write(*vn, buf, sizeof(buf), 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(sizeof(buf)), written);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Writing zero bytes to /dev/random returns zero
// Input: vnode->ops->write on /dev/random with count=0
// Expect: Returns 0
JARVIS_TEST(dev_random_write_zero, "PRE: vfsd, iocd | POST: none") {
    Vnode *vn = vfs::resolve("/dev/random");
    JARVIS_ASSERT(vn != nullptr);

    int64_t written = vn->ops->write(*vn, nullptr, 0, 0);
    JARVIS_ASSERT_EQ(0LL, written);

    JARVIS_TEST_PASS();
}

void register_random_vfs_write_tests() {
    Logger::info("Registering /dev/random VFS write tests");
    JARVIS_REGISTER_TEST(dev_random_write);
    JARVIS_REGISTER_TEST(dev_random_write_zero);
}
