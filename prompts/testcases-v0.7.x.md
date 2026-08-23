# Test Cases — v0.7.x (Phase 8: Microkernel — VFS & Drivers)

## Branch: testbed only

*Outline — test details to be expanded when implementation begins.*

### Externalise VFS — test_vfsd_server.cpp
- `vfsd` becomes exclusive owner of mount table; no in-kernel `syscall_path_open` shortcut
- All filesystem drivers (FAT32, tmpfs) run as separate ring-3 servers, routed through `vfsd` via IPC
- File open/read/write/close from userspace goes through vfsd IPC round-trip
- Mount/unmount requested via IPC to vfsd

### Externalise Block I/O — test_atad_server.cpp
- Block layer: `atad` user-space driver
- Block I/O exposed via IPC channels (iopl + shared pages)
- Read/write sector through atad server
- Multiple clients can queue block requests simultaneously

### Externalise Keyboard — test_kbd_drv.cpp
- Keyboard → `kbd_drv` user-space server
- Kernel forwards interrupt to driver IPC port
- Key press/release events delivered to kbd_drv via IPC
- kbd_drv serves keystrokes to shell or other consumers

### Externalise Framebuffer — test_fb_drv.cpp
- Framebuffer → `fb_drv` user-space server
- MMIO region delegated to fb_drv via capability
- fb_drv handles mode set, pixel write, scroll
- Double-buffering managed in userspace

### Externalise Timer/RTC — test_timer_drv.cpp
- Timer/RTC → `timer_drv` user-space server
- RTC read (time/date) served via IPC
- Timer wakeup alarms routed through timer_drv

### Driver Manager — test_drv_mgr.cpp
- Driver manager daemon enumerates PCI devices
- Driver manager spawns driver servers on demand
- Driver manager manages MMIO/capability delegation to driver servers
- Device hotplug triggers driver spawn/teardown
- Driver crash: manager detects and restarts
