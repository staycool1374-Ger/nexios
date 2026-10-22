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
  printf "\n[399-A64] clone entered\n"
  enable 6
  continue
end

# 2: user-page deep copy entered?
break kernel::VMM::deep_copy_user_pages
commands
  silent
  printf "[399-A64] deep_copy_user_pages entered\n"
  continue
end

# 3: parent reached teardown?
break kernel::TaskControlBlock::complete_teardown
commands
  silent
  printf "[399-A64] complete_teardown entered\n"
  continue
end

# 4: bogus-phys free — fires only if 0x80000000 is freed (H1b path)
break kernel::PMM::free_page if phys_addr == 0x80000000
commands
  silent
  printf "\n[399-A64] FAKE-PHYS FREE(0x80000000)\n"
  bt 8
  continue
end

# 5: first fault on the clone path — decisive for H1a (HDDM hole read)
# default_exception records ESR/FAR then skips the faulting insn
break default_exception
commands
  silent
  printf "\n[399-A64] FAULT via default_exception\n"
  printf "esr_el1 = "
  p/x $esr_el1
  printf "far_el1 = "
  p/x $far_el1
  printf "elr_el1 = "
  p/x $elr_el1
  bt 10
  quit 0
end

# 6: tick sampler — parked-PC trajectory (enabled at clone entry only,
# so boot ticks don't stall GDB). $elr_el1 is the interrupted PC.
break kernel::Scheduler::on_tick
commands
  silent
  set $n399 = $n399 + 1
  printf "[399-A64-T%d]\n", $n399
  info registers elr_el1 esr_el1 far_el1
  if $n399 >= 8
    printf "[399-A64] sampler done — task state:\n"
    tasks
    quit 0
  end
  continue
end
disable 6

# 7: panic capture
break panic
commands
  silent
  printf "\n[399-A64] PANIC (see serial log for message)\n"
  bt 16
  tasks
  shell touch build/gdb-panic-captured
  quit 1
end

printf "[399-A64] armed, resuming guest\n"
continue
printf "[399-A64] guest exited cleanly (no wedge reproduced)\n"
quit 0
