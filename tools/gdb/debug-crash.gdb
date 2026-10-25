set pagination off
set confirm off
# ELF is passed on the gdb command line; only the stub port belongs here,
# so gdb_run.py --gdb-port can rewrite it (busy :1234 auto-picks).
target extended-remote :1234

# Break at process_replenishments to catch the -1 this pointer
b *SporadicServer::process_replenishments(unsigned long long)+0x1c
commands
  silent
  printf "process_replenishments called with this=0x%llx\n", $rdi
  if $rdi == 0xFFFFFFFFFFFFFFFF
    printf "CORRUPT sporadic_server detected! Backtrace:\n"
    bt
    printf "Scheduler tasks:\n"
    # Walk the task list
    set $tc = *(unsigned long long*)0xFFFF8000005338C0
    printf "task_count_ = %llu\n", $tc
    quit
  end
  continue
end

# Alternative: break at the crash point (page fault handler)
b *0xFFFF800000204D40
commands
  silent
  printf "In fault handler\n"
  bt
  info registers
  continue
end

continue
