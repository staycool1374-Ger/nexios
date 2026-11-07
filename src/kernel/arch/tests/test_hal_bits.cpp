/// @file test_hal_bits.cpp
/// @brief HAL bit-manipulation utility tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/hal/bits.hpp>

using namespace kernel;

JARVIS_TEST(hal_bits_find_highest_zero, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(0);
    JARVIS_ASSERT_EQ(0ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_lsb, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(1ULL);
    JARVIS_ASSERT_EQ(0ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_msb, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(1ULL << 63);
    JARVIS_ASSERT_EQ(63ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_middle, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(0xFF00ULL);
    JARVIS_ASSERT_EQ(15ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_all, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(0xFFFFFFFFFFFFFFFFULL);
    JARVIS_ASSERT_EQ(63ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_multi, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(0x8000000000000001ULL);
    JARVIS_ASSERT_EQ(63ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_lowest_zero, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_lowest_bit(0);
    JARVIS_ASSERT_EQ(0ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_lowest_lsb, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_lowest_bit(1ULL);
    JARVIS_ASSERT_EQ(0ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_lowest_msb, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_lowest_bit(1ULL << 63);
    JARVIS_ASSERT_EQ(63ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_lowest_multi, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_lowest_bit(0x8000000000000001ULL);
    JARVIS_ASSERT_EQ(0ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_lowest_range, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_lowest_bit(0xF0ULL);
    JARVIS_ASSERT_EQ(4ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_range, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(0x0F00ULL);
    JARVIS_ASSERT_EQ(11ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_highest_low_64, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_highest_bit(0x8000000000000000ULL);
    JARVIS_ASSERT_EQ(63ULL, result);
    JARVIS_TEST_PASS();
}

JARVIS_TEST(hal_bits_find_lowest_low_64, "PRE: none | POST: none") {
    uint64_t result = hal::bits::find_lowest_bit(0x8000000000000000ULL);
    JARVIS_ASSERT_EQ(63ULL, result);
    JARVIS_TEST_PASS();
}

void register_hal_bits_tests() {
    Logger::info("Registering HAL bits tests");
    JARVIS_REGISTER_TEST(hal_bits_find_highest_zero);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_lsb);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_msb);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_middle);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_all);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_multi);
    JARVIS_REGISTER_TEST(hal_bits_find_lowest_zero);
    JARVIS_REGISTER_TEST(hal_bits_find_lowest_lsb);
    JARVIS_REGISTER_TEST(hal_bits_find_lowest_msb);
    JARVIS_REGISTER_TEST(hal_bits_find_lowest_multi);
    JARVIS_REGISTER_TEST(hal_bits_find_lowest_range);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_range);
    JARVIS_REGISTER_TEST(hal_bits_find_highest_low_64);
    JARVIS_REGISTER_TEST(hal_bits_find_lowest_low_64);
}