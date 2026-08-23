# Test Cases — v0.9.x (Phase 9: Hardware Drivers & Protocols)

## Branch: testbed only

*Outline — test details to be expanded when implementation begins.*

### Network Stack — test_net.cpp
- UDP socket lifecycle — Create, bind, send, and receive UDP datagrams; socket close releases resources
- ARP request/reply for gateway IP — ARP request sent to gateway; reply received and cache entry populated
- ARP cache expiration — Cached ARP entry expires after timeout; subsequent lookup triggers new ARP request
- TCP socket lifecycle — Create, bind, listen, accept, connect, send, recv, close
- TCP retransmission — Lost packet triggers retransmit within RTO
- TCP congestion control — Window scales under loss
- ICMP echo (ping) — Kernel responds to ping requests

### USB Stack — test_usb.cpp
- UHCI/EHCI/xHCI controller detected and initialized
- USB keyboard enumerated and delivers keystrokes
- USB mouse enumerated and delivers pointer events
- USB storage enumerated and serves block I/O
- Hotplug: device insertion triggers enumeration
- Hotplug: device removal triggers clean teardown
- PS/2 input replaced by USB HID on hardware with USB controllers

### Hot-Path Secure Call Sequence — test_call_sequence.cpp
- Call sequence enforcer validates X→Y→Z ordering at compile time or runtime
- Invalid sequence (Y→Z→X) rejected with error
- Zero overhead in production builds (CONFIG_SECURE_CALL_SEQUENCE=0)
- Typestate pattern or linear-handle approach enforced
- Device init/teardown sequences validated
- IPC send/recv handshake protocol validated
- MMIO map/unmap guard prevents double-map or unmapped access
