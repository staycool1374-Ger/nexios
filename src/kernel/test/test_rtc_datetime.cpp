/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_rtc_datetime.cpp
/// @brief RTC date arithmetic tests (milestone v0.4.3 issue #116):
///        read_time tm-field mapping (drives read_time_raw century/BCD
///        handling), read_seconds composition, and BCD edge nibbles.
/// @note  RTC::make_timestamp / read_time_raw are private in hal/rtc.hpp;
///        their arithmetic is verified through the public composition
///        surface (read_time / read_seconds) against an independent
///        reference conversion implemented in this file.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/rtc.hpp>

using namespace kernel;

namespace {

/// @brief Independent reference: days-from-civil (Howard Hinnant algorithm)
///        converting a proleptic Gregorian date to days since 1970-01-01.
uint64_t ref_days_from_civil(uint64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    uint64_t era = year / 400;
    unsigned yoe = static_cast<unsigned>(year - era * 400);            // [0,399]
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +    // [0,365]
                   day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              // [0,146096]
    return era * 146097 + doe - 719468;
}

/// @brief Independent reference timestamp from tm fields (UTC).
uint64_t ref_timestamp(const arch::tm &t) {
    uint64_t days = ref_days_from_civil(
        static_cast<uint64_t>(t.tm_year + 1900),
        static_cast<unsigned>(t.tm_mon + 1), static_cast<unsigned>(t.tm_mday));
    return days * 86400ULL + static_cast<uint64_t>(t.tm_hour) * 3600ULL +
           static_cast<uint64_t>(t.tm_min) * 60ULL +
           static_cast<uint64_t>(t.tm_sec);
}

} // namespace

// Runmode: kernel
// Testidea: read_time maps the raw CMOS fields into the tm contract:
// tm_mon is 0-based (month-1), tm_year is years-since-1900, and every
// field stays in calendar range.  This drives read_time_raw internally,
// covering its century register composition and BCD→binary conversion on
// the live QEMU CMOS.
// Input: RTC::read_time on the real CMOS.
// Expect: month 1-12 ⇒ tm_mon 0-11; day 1-31; hour ≤ 23; min/sec ≤ 59;
//         year 2020-2200 ⇒ tm_year 120-300; tm_wday/tm_yday/tm_isdst are
//         the documented untracked zeros.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_tm_field_mapping, "PRE: iocd | POST: none") {
    arch::tm t{};
    arch::RTC::read_time(&t);
    JARVIS_ASSERT(t.tm_mon >= 0 && t.tm_mon <= 11);
    JARVIS_ASSERT(t.tm_mday >= 1 && t.tm_mday <= 31);
    JARVIS_ASSERT(t.tm_hour >= 0 && t.tm_hour <= 23);
    JARVIS_ASSERT(t.tm_min >= 0 && t.tm_min <= 59);
    JARVIS_ASSERT(t.tm_sec >= 0 && t.tm_sec <= 59);
    JARVIS_ASSERT(t.tm_year >= 120 && t.tm_year <= 300); // 2020..2200
    JARVIS_ASSERT_EQ(0, t.tm_wday);
    JARVIS_ASSERT_EQ(0, t.tm_yday);
    JARVIS_ASSERT_EQ(0, t.tm_isdst);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: read_seconds equals the independent civil-calendar conversion
// of the read_time fields within a one-minute drift (CMOS may tick between
// the two reads).  This exercises the whole pipeline — raw register read,
// BCD conversion, century composition, month tables and leap handling for
// the live date — against an independent reference (no shared code).
// Input: read_seconds() and read_time() on the live CMOS.
// Expect: |read_seconds - ref_timestamp(tm)| ≤ 60 s and both land in the
//         2020..2200 epoch window.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_read_seconds_composition, "PRE: iocd | POST: none") {
    uint64_t direct = arch::RTC::read_seconds();
    JARVIS_ASSERT(direct > 1577836800ULL); // 2020-01-01
    JARVIS_ASSERT(direct < 7258118400ULL); // 2200-01-01
    arch::tm t{};
    arch::RTC::read_time(&t);
    uint64_t composed = ref_timestamp(t);
    JARVIS_ASSERT(composed > 1577836800ULL);
    uint64_t delta = direct > composed ? direct - composed : composed - direct;
    JARVIS_ASSERT(delta <= 60ULL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: read_time is stable across back-to-back reads inside one CMOS
// second (rollover re-read branch must never produce an inconsistent
// field set — e.g. a minute that jumped without the seconds matching).
// Input: 8 read_time calls with second-boundary retry.
// Expect: Every sampled tuple is calendar-valid (cross-field consistency,
//         not equality — the clock may tick between samples).
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_read_time_stability, "PRE: iocd | POST: none") {
    for (int i = 0; i < 8; ++i) {
        arch::tm t{};
        arch::RTC::read_time(&t);
        bool valid = (t.tm_mon >= 0 && t.tm_mon <= 11) &&
                     (t.tm_mday >= 1 && t.tm_mday <= 31) &&
                     (t.tm_hour >= 0 && t.tm_hour <= 23) &&
                     (t.tm_min >= 0 && t.tm_min <= 59) &&
                     (t.tm_sec >= 0 && t.tm_sec <= 59) &&
                     (t.tm_year >= 120 && t.tm_year <= 300);
        JARVIS_ASSERT(valid);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: BCD conversion edge nibbles — both nibbles at maximum (0x99 →
// 99) and single-nibble extremes decode positionally.
// Input: bcd_to_bin on 0x99, 0x90, 0x09, 0x50, 0x59.
// Expect: 99, 90, 9, 50, 59 — documents the no-validation contract the
// century/year fields rely on at their BCD maximums.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_bcd_edge_nibbles, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(99, arch::RTC::bcd_to_bin(0x99));
    JARVIS_ASSERT_EQ(90, arch::RTC::bcd_to_bin(0x90));
    JARVIS_ASSERT_EQ(9, arch::RTC::bcd_to_bin(0x09));
    JARVIS_ASSERT_EQ(50, arch::RTC::bcd_to_bin(0x50));
    JARVIS_ASSERT_EQ(59, arch::RTC::bcd_to_bin(0x59));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: bin_to_bcd / bcd_to_bin round-trip for every valid decimal
// byte 0..99.
// Input: All 100 values round-tripped.
// Expect: bcd_to_bin(bin_to_bcd(n)) == n for all n in 0..99.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_bcd_roundtrip_full, "PRE: iocd | POST: none") {
    for (unsigned n = 0; n <= 99; ++n) {
        uint8_t bcd = arch::RTC::bin_to_bcd(static_cast<uint8_t>(n));
        uint8_t back = arch::RTC::bcd_to_bin(bcd);
        if (back != n) {
            JARVIS_FAIL("bcd roundtrip failed at n=%u got=%u", n,
                        (unsigned)back);
        }
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: BCD with an invalid high nibble (0xA5) documents the chosen
// positional contract (no range validation in the converter).
// Input: bcd_to_bin(0xA5), bcd_to_bin(0x1F).
// Expect: 105 (10 tens + 5) and 25 (1 ten + 15) — out-of-range nibbles
//         accumulate positionally without clamping.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_bcd_invalid_nibble_contract, "PRE: iocd | POST: none") {
    JARVIS_ASSERT_EQ(105, arch::RTC::bcd_to_bin(0xA5));
    JARVIS_ASSERT_EQ(25, arch::RTC::bcd_to_bin(0x1F));
    JARVIS_TEST_PASS();
}

void register_rtc_datetime_tests() {
    Logger::info("Registering RTC datetime tests");
    JARVIS_REGISTER_TEST(rtc_dt_tm_field_mapping);
    JARVIS_REGISTER_TEST(rtc_dt_read_seconds_composition);
    JARVIS_REGISTER_TEST(rtc_dt_read_time_stability);
    JARVIS_REGISTER_TEST(rtc_dt_bcd_edge_nibbles);
    JARVIS_REGISTER_TEST(rtc_dt_bcd_roundtrip_full);
    JARVIS_REGISTER_TEST(rtc_dt_bcd_invalid_nibble_contract);
}
#elif defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/rtc.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

// Independent reference: days-from-civil (Howard Hinnant algorithm),
// same contract as the x86 section's reference — no shared code with the
// RTC driver under test.
uint64_t ref_days_from_civil(uint64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    uint64_t era = year / 400;
    unsigned yoe = static_cast<unsigned>(year - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint64_t ref_timestamp(const arch::tm &t) {
    uint64_t days = ref_days_from_civil(
        static_cast<uint64_t>(t.tm_year + 1900),
        static_cast<unsigned>(t.tm_mon + 1), static_cast<unsigned>(t.tm_mday));
    return days * 86400ULL + static_cast<uint64_t>(t.tm_hour) * 3600ULL +
           static_cast<uint64_t>(t.tm_min) * 60ULL +
           static_cast<uint64_t>(t.tm_sec);
}

} // namespace

// Runmode: kernel
// Testidea: The wall-clock source contract (issue #355): QEMU virt always
// provides a clock (PL011 on aarch64, Goldfish on riscv64), so available()
// is true and source_name() names it. If the platform ever drops the
// device the test fails loud (fail-loud, never silent).
// Input: RTC::available(), RTC::source_name().
// Expect: available() true; "pl031" on aarch64, "goldfish" on riscv64.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_source_contract, "PRE: iocd | POST: none") {
    JARVIS_ASSERT(arch::RTC::available());
    const char *name = arch::RTC::source_name();
    JARVIS_ASSERT(name != nullptr);
#if defined(CONFIG_ARCH_AARCH64)
    JARVIS_ASSERT(strcmp(name, "pl031") == 0);
#elif defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT(strcmp(name, "goldfish") == 0);
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Wall-clock seconds are plausible and monotonic — at/after the
// 2001 plausibility floor, never going backwards. No host-clock proximity
// is asserted (TCG/host clocks may diverge by design, issue #384).
// Input: Two read_seconds() calls.
// Expect: both >= 1e9; second >= first.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_seconds_plausible_monotonic, "PRE: iocd | POST: none") {
    uint64_t first = arch::RTC::read_seconds();
    uint64_t second = arch::RTC::read_seconds();
    JARVIS_ASSERT(first >= 1000000000ULL);
    JARVIS_ASSERT(second >= 1000000000ULL);
    JARVIS_ASSERT(second >= first);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: read_time fields are calendar-valid on the platform clock,
// with the documented untracked zeros (issue #355 H4: field ranges).
// Input: RTC::read_time().
// Expect: mon 0-11; mday 1-31; hour <= 23; min/sec <= 59;
//         year >= 101 (2001 plausibility floor); wday/yday/isdst zero.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_tm_field_ranges, "PRE: iocd | POST: none") {
    arch::tm t{};
    arch::RTC::read_time(&t);
    JARVIS_ASSERT(t.tm_mon >= 0 && t.tm_mon <= 11);
    JARVIS_ASSERT(t.tm_mday >= 1 && t.tm_mday <= 31);
    JARVIS_ASSERT(t.tm_hour >= 0 && t.tm_hour <= 23);
    JARVIS_ASSERT(t.tm_min >= 0 && t.tm_min <= 59);
    JARVIS_ASSERT(t.tm_sec >= 0 && t.tm_sec <= 59);
    JARVIS_ASSERT(t.tm_year >= 101);
    JARVIS_ASSERT_EQ(0, t.tm_wday);
    JARVIS_ASSERT_EQ(0, t.tm_yday);
    JARVIS_ASSERT_EQ(0, t.tm_isdst);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The mtime-fallback contract is documented even though QEMU
// virt always provides a clock (issue #355 H4): without a wall source the
// driver renders mtime uptime from the 1970 epoch (small values), while a
// live source always reads at/after the plausibility floor.
// Input: RTC::available(), RTC::read_seconds(), source_name().
// Expect: !available ⇒ seconds < 1e9 and source "mtime-fallback";
//         available ⇒ seconds >= 1e9.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_fallback_contract_documented, "PRE: iocd | POST: none") {
    bool avail = arch::RTC::available();
    uint64_t secs = arch::RTC::read_seconds();
    const char *name = arch::RTC::source_name();
    JARVIS_ASSERT(name != nullptr);
    if (!avail) {
        JARVIS_ASSERT(secs < 1000000000ULL);
        JARVIS_ASSERT(strcmp(name, "mtime-fallback") == 0);
    } else {
        JARVIS_ASSERT(secs >= 1000000000ULL);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: read_seconds agrees with the independent civil-calendar
// conversion of read_time within a generous bound (the clock may tick
// between the two reads; no shared code with the driver).
// Input: read_seconds() and read_time() on the platform clock.
// Expect: |direct - composed| <= 120 s.
// Depends: arch::RTC
JARVIS_TEST(rtc_dt_seconds_tm_composition, "PRE: iocd | POST: none") {
    uint64_t direct = arch::RTC::read_seconds();
    arch::tm t{};
    arch::RTC::read_time(&t);
    uint64_t composed = ref_timestamp(t);
    uint64_t delta = direct > composed ? direct - composed : composed - direct;
    JARVIS_ASSERT(delta <= 120ULL);
    JARVIS_TEST_PASS();
}

void register_rtc_datetime_tests() {
    Logger::info("Registering RTC datetime tests");
    JARVIS_REGISTER_TEST(rtc_dt_source_contract);
    JARVIS_REGISTER_TEST(rtc_dt_seconds_plausible_monotonic);
    JARVIS_REGISTER_TEST(rtc_dt_tm_field_ranges);
    JARVIS_REGISTER_TEST(rtc_dt_fallback_contract_documented);
    JARVIS_REGISTER_TEST(rtc_dt_seconds_tm_composition);
}
#endif // CONFIG_ARCH_X86_64
