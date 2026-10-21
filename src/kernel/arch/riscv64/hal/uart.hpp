#pragma once

#include <types.hpp>

/// @brief RISC-V 64 QEMU virt 16550A UART register literals (issue #387:
///        single arch-owned source — the driver and the arch tests share
///        these, never raw address literals).
/// @note Raw physical address, directly dereferenceable in S-mode (riscv64
///       low half is identity-mapped — same precedent as the PLIC
///       accessors).  Divisor pre-initialized by OpenSBI/QEMU (issue #247).
namespace arch {

/// @brief UART0 MMIO base (QEMU virt machine).
inline constexpr uint64_t UART0_BASE = 0x10000000ULL;
/// @brief 16550A register offsets from UART0_BASE.
inline constexpr uint64_t UART_RBR_OFF = 0; // RX buffer (read)
inline constexpr uint64_t UART_IER_OFF = 1; // interrupt enable
inline constexpr uint64_t UART_LCR_OFF = 3; // line control
inline constexpr uint64_t UART_MCR_OFF = 4; // modem control
inline constexpr uint64_t UART_LSR_OFF = 5; // line status
/// @brief Line-status bits.
inline constexpr uint8_t UART_LSR_DATA_READY = 0x01; // RX data available
inline constexpr uint8_t UART_LSR_TEMT = 0x40;      // TX holding reg empty
/// @brief Modem-control loopback bit (test-only loopback control).
inline constexpr uint8_t UART_MCR_LOOPBACK = 0x10;

} // namespace arch
