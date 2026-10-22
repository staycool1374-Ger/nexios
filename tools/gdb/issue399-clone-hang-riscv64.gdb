set confirm off
set pagination off
set print pretty on
set remotetimeout 10

source tools/gdb/kernel.py

target extended-remote :1234

set $n399 = 0

# 1: clone entry — dump the synthetic parent shape, arm the sampler
break kernel::TaskControlBlock::clone
commands
  silent
  printf "\n[399-RV64] clone entered\n"
  enable 7
  continue
end

# 2: user-page deep copy entered?
break kernel::VMM::deep_copy_user_pages
commands
  silent
  printf "[399-RV64] deep_copy_user_pages entered\n"
  continue
end

# 3: parent reached teardown?
break kernel::TaskControlBlock::complete_teardown
commands
  silent
  printf "[399-RV64] complete_teardown entered\n"
  continue
end

# 4: bogus-phys free — fires only if 0x80000000 is freed (H1b path)
break kernel::PMM::free_page if phys_addr == 0x80000000
commands
  silent
  printf "\n[399-RV64] FAKE-PHYS FREE(0x80000000)\n"
  bt 8
  continue
end

# 5: S-mode exception dispatch — first fault on the clone path (H1a)
break riscv64_exception_dispatch
commands
  silent
  printf "\n[399-RV64] FAULT via riscv64_exception_dispatch\n"
  info args
  bt 10
  quit 0
end

# 6: PLIC trap — ignore interrupts (MSB set: timer/external), quit only
# on real exceptions (MSB clear), which is the H1a signal
break handle_plic_trap
commands
  silent
  if (scause & 0x8000000000000000) == 0
    printf "\n[399-RV64] EXCEPTION via handle_plic_trap: scause="
    p/x scause
    printf " sepc="
    p/x sepc
    bt 10
    quit 0
  end
  continue
end

# 7: tick sampler — parked-PC trajectory (enabled at clone entry only,
# so boot ticks don't stall GDB). $sepc is the interrupted PC.
break kernel::Scheduler::on_tick
commands
  silent
  set $n399 = $n399 + 1
  printf "[399-RV64-T%d] pc=", $n399
  p/x $pc
  bt 3
  if $n399 >= 8
    printf "[399-RV64] sampler done — task state:\n"
    tasks
    quit 0
  end
  continue
end
disable 7

# 8: panic capture
break panic
commands
  silent
  printf "\n[399-RV64] PANIC (see serial log for message)\n"
  bt 16
  tasks
  shell touch build/gdb-panic-captured
  quit 1
end

printf "[399-RV64] armed, resuming guest\n"
continue
printf "[399-RV64] guest exited cleanly (no wedge reproduced)\n"
quit 0
