import gdb
import struct

HHDM_OFFSET = 0xFFFF800000000000

TASK_STATE_NAMES = {
    0: "READY",
    1: "RUNNING",
    2: "BLOCKED",
    3: "WAITING",
    4: "TERMINATED",
}

class TaskControlBlockPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            pid = int(self.val["id"])
            prio = int(self.val["priority"])
            state_val = int(self.val["state"])
            state_name = TASK_STATE_NAMES.get(state_val, f"UNKNOWN({state_val})")
            return f"TCB(pid=0x{pid:x}, prio={prio}, state={state_name})"
        except Exception:
            return str(self.val)

class TaskContextPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            rip = int(self.val["rip"])
            rsp = int(self.val["rsp"])
            return f"TaskContext(rip=0x{rip:x}, rsp=0x{rsp:x})"
        except Exception:
            return str(self.val)

class PhysicalAddressPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            addr = int(self.val["addr_"])
            return f"PhysicalAddress(0x{addr:x})"
        except Exception:
            return str(self.val)

class VirtualAddressPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            addr = int(self.val["addr_"])
            tag = "KERN" if addr >= 0xFFFF800000000000 else "USER" if addr < 0x0000800000000000 else "INVAL"
            return f"VirtualAddress(0x{addr:x} [{tag}])"
        except Exception:
            return str(self.val)

class PageAddressPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            addr = int(self.val["addr_"])
            return f"PageAddress(0x{addr:x})"
        except Exception:
            return str(self.val)

def _type_name(val):
    t = val.type
    if t.code == gdb.TYPE_CODE_REF:
        t = t.target()
    return t.tag or ""

def jarvis_pretty_printer(val):
    tag = _type_name(val)
    if tag == "kernel::TaskControlBlock":
        return TaskControlBlockPrinter(val)
    if tag == "kernel::TaskContext":
        return TaskContextPrinter(val)
    if tag == "kernel::PhysicalAddress":
        return PhysicalAddressPrinter(val)
    if tag == "kernel::VirtualAddress":
        return VirtualAddressPrinter(val)
    if tag == "kernel::PageAddress":
        return PageAddressPrinter(val)
    return None

gdb.pretty_printers.append(jarvis_pretty_printer)

def _sched_var(name):
    return gdb.parse_and_eval(f"'kernel::Scheduler::{name}'")

def _iter_all_tasks():
    """Yield (bucket_prio, tcb_value) over AllTasksRegistry buckets.

    The linear tasks_[]/task_count_ era is over (all_tasks_ intrusive
    per-priority lists); walk heads_ + pri_next_ with cycle + hop caps
    so a corrupted link cannot hang the debugger.
    """
    reg = gdb.parse_and_eval("'kernel::Scheduler::all_tasks_'")
    heads = reg["heads_"]
    lo, hi = heads.type.range()
    seen = set()
    n = 0
    for prio in range(int(hi), int(lo) - 1, -1):
        try:
            node = heads[int(prio)]
        except Exception:
            continue
        hops = 0
        while n < 256 and hops < 512:
            try:
                if node == 0:
                    break
                addr = int(node)
            except Exception:
                break
            if addr in seen:
                break
            seen.add(addr)
            try:
                tcb = node.dereference()
            except Exception:
                break
            yield int(prio), tcb
            n += 1
            try:
                node = tcb["pri_next_"]
            except Exception:
                break
            hops += 1

def _tcb_name(tcb):
    try:
        return tcb["name"].string(encoding="utf-8", errors="replace")
    except Exception:
        return "?"

class TasksCommand(gdb.Command):
    """List all kernel tasks with state, priority, PID."""

    def __init__(self):
        super().__init__("tasks", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            rows = list(_iter_all_tasks())
        except Exception as e:
            gdb.write(f"Error: cannot read scheduler state ({e})\n")
            return

        gdb.write(f"{'Idx':>4} {'PID':>8} {'State':<12} {'Prio':>4} {'Name':<16}\n")
        gdb.write("-" * 48 + "\n")
        gdb.flush()

        for i, (bucket, tcb) in enumerate(rows):
            try:
                pid = int(tcb["id"])
                state_val = int(tcb["state"])
                state_name = TASK_STATE_NAMES.get(state_val, f"UNKNOWN({state_val})")
                prio = int(tcb["priority"])
                name = _tcb_name(tcb)
                gdb.write(f"{i:>4}  0x{pid:<6x} {state_name:<12} {prio:>4}  {name:<16}\n")
            except Exception as e:
                gdb.write(f"  {i:>4} error: {e}\n")
        gdb.flush()

class TaskCommand(gdb.Command):
    """Show detailed info for a task by index or PID."""

    def __init__(self):
        super().__init__("task", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        args = arg.strip().split()
        if not args:
            gdb.write("Usage: task <index|pid>\n")
            return
        try:
            q = args[0]
            query = int(q, 16) if q.startswith("0x") else int(q, 10)
        except ValueError:
            gdb.write(f"Invalid: {args[0]}\n")
            return

        try:
            rows = list(_iter_all_tasks())
        except Exception as e:
            gdb.write(f"Error: {e}\n")
            return

        found = None
        for i, (bucket, t) in enumerate(rows):
            try:
                if i == query or int(t["id"]) == query:
                    found = (i, t)
                    break
            except Exception:
                continue

        if found is None:
            gdb.write(f"No task for '{arg}'\n")
            return

        idx, tcb = found
        pid = int(tcb["id"])
        parent = int(tcb["parent_id"])
        sv = int(tcb["state"])
        sn = TASK_STATE_NAMES.get(sv, f"UNK({sv})")
        prio = int(tcb["priority"])
        bp = int(tcb["base_priority"])
        period = int(tcb["period_ticks"])
        dl = int(tcb["deadline_ticks"])
        et = int(tcb["executed_ticks"])
        rm = int(tcb["remaining_ticks"])
        ec = int(tcb["exit_code"])
        kst = int(tcb["kernel_stack_top"])
        pt = int(tcb["page_table_"])
        ustk = int(tcb["user_stack_"])
        nch = int(tcb["num_children"])
        ctx = tcb["context"]
        rip = int(ctx["rip"])
        rsp = int(ctx["rsp"])
        rbp = int(ctx["rbp"])

        gdb.write(f"Task [{idx}]:\n")
        gdb.write(f"  PID:        0x{pid:x}\n")
        gdb.write(f"  Parent:     0x{parent:x}\n")
        gdb.write(f"  State:      {sn} ({sv})\n")
        gdb.write(f"  Priority:   {prio} (base: {bp})\n")
        gdb.write(f"  Period:     {period}  Deadline: {dl}\n")
        gdb.write(f"  ExecTicks:  {et}  Remaining: {rm}\n")
        gdb.write(f"  ExitCode:   0x{ec:x}\n")
        gdb.write(f"  KS top:     0x{kst:x}  PT phys: 0x{pt:x}\n")
        gdb.write(f"  User stack: 0x{ustk:x}  Children: {nch}\n")
        gdb.write(f"  RIP: 0x{rip:x}  RSP: 0x{rsp:x}  RBP: 0x{rbp:x}\n")


class RegsCommand(gdb.Command):
    """Print general-purpose + control registers with RFLAGS decode."""

    def __init__(self):
        super().__init__("regs", gdb.COMMAND_USER)

    def _r(self, name):
        try:
            return int(gdb.parse_and_eval(f"${name}"))
        except Exception:
            return None

    def invoke(self, arg, from_tty):
        regs = {r: self._r(r) for r in ["rax","rbx","rcx","rdx","rsi","rdi",
            "rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15","rip","eflags"]}
        if regs["rip"] is None:
            gdb.write("Cannot read registers (target not connected?)\n")
            return

        gdb.write("  GPRs:\n")
        pairs = [("RAX","RBX"),("RCX","RDX"),("RSI","RDI"),("RBP","RSP"),
                 ("R8","R9"),("R10","R11"),("R12","R13"),("R14","R15")]
        for a, b in pairs:
            va = regs[a.lower()] or 0
            vb = regs[b.lower()] or 0
            gdb.write(f"  {a}: 0x{va:016x}  {b}: 0x{vb:016x}\n")

        gdb.write(f"\n  RIP: 0x{regs['rip']:016x}\n")
        fl = regs["eflags"] or 0
        fs = "".join([
            "C" if fl & 0x001 else "c",
            "P" if fl & 0x004 else "p",
            "A" if fl & 0x010 else "a",
            "Z" if fl & 0x040 else "z",
            "S" if fl & 0x080 else "s",
            "T" if fl & 0x100 else "t",
            "I" if fl & 0x200 else "i",
            "D" if fl & 0x400 else "d",
            "O" if fl & 0x800 else "o",
        ])
        gdb.write(f"  RFLAGS: 0x{fl:08x} ({fs})\n")

        for cr in ["cr0","cr2","cr3","cr4"]:
            v = self._r(cr)
            if v is not None:
                gdb.write(f"  {cr.upper()}: 0x{v:016x}\n")


class PanicInfoCommand(gdb.Command):
    """Dump current task, backtrace, and registers at the stop point."""

    def __init__(self):
        super().__init__("panicinfo", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            cur = gdb.parse_and_eval("'kernel::Scheduler::current_task'()")
            if cur:
                gdb.write("Current Task:\n")
                gdb.write(f"  {cur}\n\n")
        except Exception:
            pass
        gdb.write("Backtrace:\n")
        try:
            gdb.execute("bt 32")
        except Exception:
            pass
        gdb.write("\n")
        try:
            gdb.execute("regs")
        except Exception:
            pass


class Pml4Command(gdb.Command):
    """Walk 4-level page table from CR3 (default) or given phys address."""

    def __init__(self):
        super().__init__("pml4", gdb.COMMAND_USER)

    def _r8(self, va):
        try:
            p = gdb.Value(va).cast(gdb.lookup_type("uint64_t").pointer())
            return int(p.dereference())
        except Exception:
            return None

    def _walk(self, phys, label, depth, base_va, verbose):
        va = phys + HHDM_OFFSET
        indent = "  " * depth
        count = 0
        for i in range(512):
            e = self._r8(va + i * 8)
            if e is None:
                break
            if not (e & 1):
                continue
            count += 1
            if verbose or count <= 24:
                pa = e & 0x000FFFFFFFFFF000
                a = ""
                a += "W" if e & 2 else "R"
                a += "U" if e & 4 else "S"
                a += "X" if not (e & 0x40) else "x"
                if e & 0x80:
                    a += " L"
                v = base_va + (i << (12 + depth * 9))
                gdb.write(f"{indent}[{i:>3}] 0x{pa:016x} [{a}] 0x{v:016x}\n")
        if count > 24 and not verbose:
            gdb.write(f"{indent}... ({count} entries, use pml4 -v for all)\n")
        return count

    def invoke(self, arg, from_tty):
        args = arg.strip().split()
        verbose = "-v" in args
        try:
            cr3 = int(gdb.parse_and_eval("$cr3")) & 0x000FFFFFFFFFF000
        except Exception:
            gdb.write("Not connected.\n")
            return

        gdb.write(f"CR3=0x{cr3:x}  PML4 @ phys=0x{cr3:x}\n")
        n4 = self._walk(cr3, "PML4", 0, 0, verbose)
        gdb.write(f"  PML4: {n4} entries\n")

        if n4:
            va4 = cr3 + HHDM_OFFSET
            for i in range(512):
                e = self._r8(va4 + i * 8)
                if e and (e & 1):
                    p3 = e & 0x000FFFFFFFFFF000
                    n3 = self._walk(p3, "PDPT", 1, i << 39, verbose)
                    if n3:
                        gdb.write(f"  PML4[{i}] PDPT: {n3} entries\n")


TasksCommand()
TaskCommand()
RegsCommand()
PanicInfoCommand()
Pml4Command()

gdb.write("[Jarvis GDB] Plugin loaded. Commands: tasks, task <n>, regs, panicinfo, pml4\n")
