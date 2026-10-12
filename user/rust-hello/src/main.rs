// NexIOS Rust hello-world (issue #273).
//
// Minimal `#![no_std]`, `#![no_main]`, `panic = "abort"`, core-only user
// program for the x86_64-nexios target. Prints one line via the WRITE
// syscall and exits 0.
//
// Syscall ABI (single source of truth: src/libc/syscall.h):
//   `int $0x80`, number in rax, args in rbx/rcx/rdx/rsi.
//   SYS_WRITE = 13 (fd, buf, len) — src/libc/syscall.h:46.
//   SYS_EXIT  = 6  (code)          — src/libc/syscall.h:39.
// NOTE: SYS_KLOG (48) is a dmesg READ/clear, not console print
// (syscall_handlers_misc.cpp) — hello output uses WRITE on fd 1, which
// the loader wires to the console (open_std_fds, elf.cpp).
#![no_std]
#![no_main]

use core::arch::global_asm;
use core::panic::PanicInfo;

// ---------------------------------------------------------------------------
// Raw trap trampoline.
//
// Rust `asm!` forbids rbx (LLVM-reserved), but the NexIOS ABI takes arg0
// in rbx — so the trap lives in raw assembly, which saves/restores rbx
// explicitly (rbx is callee-saved under SysV; the two pushes keep the
// stack 16-aligned). SysV inputs: rdi=nr, rsi=a0, rdx=a1, rcx=a2.
// NexIOS inputs: rax=nr, rbx=a0, rcx=a1, rdx=a2. Returns rax.
// ---------------------------------------------------------------------------
global_asm!(
    ".text",
    ".global sys_trap4",
    ".type sys_trap4, @function",
    "sys_trap4:",
    "push %rbx",
    "mov %rdi, %rax",
    "mov %rsi, %rbx",
    "push %rcx", // stash a2
    "mov %rdx, %rcx", // rcx = a1
    "pop %rdx", // rdx = a2
    "int $0x80",
    "pop %rbx",
    "ret",
    options(att_syntax)
);

extern "C" {
    fn sys_trap4(nr: u64, a0: u64, a1: u64, a2: u64) -> u64;
}

/// WRITE(fd, buf, len) — returns bytes written or (u64)-1 on error.
#[inline(always)]
unsafe fn sys_write(fd: u64, buf: *const u8, len: u64) -> u64 {
    sys_trap4(13, fd, buf as u64, len)
}

/// EXIT(code) — never returns.
#[inline(always)]
unsafe fn sys_exit(code: u64) -> ! {
    sys_trap4(6, code, 0, 0);
    core::hint::unreachable_unchecked()
}

// ---------------------------------------------------------------------------
// Entry point (mirrors src/libc/crt0.S: _start -> main -> exit).
// ---------------------------------------------------------------------------
global_asm!(
    ".text",
    ".global _start",
    ".type _start, @function",
    "_start:",
    // Align the stack, call rust_main, exit with its return code.
    "and $-16, %rsp",
    "call rust_main",
    "mov %rax, %rdi",
    "call sys_exit_rust",
    // Unreachable fallback (mirrors crt0's hlt).
    "hlt",
    ".global sys_exit_rust",
    ".type sys_exit_rust, @function",
    "sys_exit_rust:",
    "mov %rdi, %rbx",
    "mov $6, %rax", // SYS_EXIT
    "int $0x80",
    "hlt",
    options(att_syntax)
);

// ---------------------------------------------------------------------------
// Program.
// ---------------------------------------------------------------------------

/// Called from `_start`; returns the process exit code.
#[no_mangle]
pub extern "C" fn rust_main() -> u64 {
    let msg = b"Hello from Rust on NexIOS\n";
    unsafe {
        sys_write(1, msg.as_ptr(), msg.len() as u64);
    }
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe {
        sys_exit(1);
    }
}
