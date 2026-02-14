# RetroArch Netplay Compatibility - Feasibility Assessment

## Context

NextUI has 3 multiplayer link types, all using custom protocols incompatible with RetroArch. When connecting to RetroArch, the first 4 bytes NextUI sends don't match RetroArch's expected magic (`0x52414E50` / "RANP"), causing immediate rejection: *"peer is not running RetroArch."*

The goal: assess whether NextUI can switch to RetroArch's netplay protocol so NextUI devices can play with RetroArch devices (and with each other using the same protocol).

---

## 1. GBA Link (gbalink.c) - gpSP / Pokemon

**Verdict: FEASIBLE - Moderate effort**

### Why it works
Both NextUI and RetroArch use the same libretro API (`RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE` / env call 78). The gpSP core handles all game-level protocol (RFU wireless adapter emulation) identically on both sides. The frontends are just transport layers wrapping core packets over TCP.

### What changes

| Current (NextUI) | Target (RetroArch) |
|---|---|
| Magic: `0x47424C4B` | Magic: `0x52414E50` |
| Handshake: `CMD_READY` exchange | Handshake: 24-byte header + nick + info + sync |
| Packet format: 5-byte header (u8 cmd, u16 size, u16 client_id) | Packet format: 8-byte header (u32 cmd, u32 size) |
| Core packets: `CMD_SIO_DATA` (0x01) | Core packets: `NETPLAY_CMD_NETPACKET` (0x0048) |
| Heartbeat: custom `CMD_HEARTBEAT` | Heartbeat: not needed (RA has its own) |
| Discovery: UDP `GBDQ`/`GBDR` on port 55438 | Discovery: RA uses lobby server or LAN broadcast with different format |

### Implementation scope
1. **New module `ra_protocol.c/h`** (~400-600 lines): RA handshake (24-byte header exchange, nick/info/sync commands), RA command send/recv, NETPACKET wrapping/unwrapping
2. **Modify `gbalink.c`** (~200-300 lines changed): Replace handshake in host listen thread and client connect, replace send/recv packet functions to use RA wire format
3. **Modify `minarch.c`** (~10 lines): Expose core library name/version for RA `CMD_INFO` exchange
4. **Modify `netplay_helper.c`** (~50 lines): Update discovery to support RA LAN discovery format, or add manual IP entry for RA hosts
5. **Modify makefile** (~1 line): Add ra_protocol.c

### RA Handshake details needed
- **Client sends**: 24-byte header (magic, platform_magic, compression=0, HIGH_PROTOCOL_VERSION=7, protocol_version=7, impl_magic)
- **Server sends**: 24-byte header (magic, platform_magic, compression=0, salt=0, protocol_version=7, impl_magic)
- **Both exchange**: `CMD_NICK` (0x0020, 32-byte nickname)
- **Client sends**: `CMD_INFO` (0x0022, content_crc + core_name[32] + core_version[32])
- **Server sends**: `CMD_SYNC` (0x0023, frame_count + client_number + device configs + no SRAM for netpacket mode)
- **Then**: Core packets flow via `CMD_NETPACKET` (0x0048, payload = u32 client_id + packet_data)

### Risks
- RA `CMD_SYNC` payload format for netpacket mode needs exact reproduction (device configs, share modes, device-client mapping). Must match RA source precisely.
- Platform magic mismatch may cause warnings if RA considers the core platform-dependent
- RA protocol version 5-7 must be correctly negotiated
- Discovery: NextUI UDP broadcast won't be seen by RA (different magic/port). Need manual IP or adapt RA's discovery format.

### Estimated effort: ~800-1000 lines new/changed code

---

## 2. Input-Sync Netplay (netplay.c) - fbneo, fceumm, snes9x, etc.

**Verdict: NOT FEASIBLE without major rearchitecture**

### The fundamental problem: Rollback vs Lockstep

| | RetroArch | NextUI |
|---|---|---|
| Sync model | **Rollback** (speculative execution + rewind + replay) | **Lockstep** (stall until input arrives) |
| State management | Ring buffer of savestates, one per frame | No per-frame savestates |
| Late input handling | Rewind to frame, replay with correct input | Stall and wait |
| CRC verification | Per-frame CRC check via `CMD_CRC` (0x0040) | None |
| Desync recovery | Load savestate via `CMD_LOAD_SAVESTATE` (0x0042) | Disconnect |

These are **architecturally incompatible**. You cannot have one side doing rollback and the other doing lockstep - they will immediately desync because:
1. RA speculatively runs frames before receiving remote input. NextUI waits.
2. RA expects CRC responses every frame. NextUI doesn't compute CRCs.
3. RA may send rollback corrections. NextUI can't rewind.
4. Frame advancement timing differs fundamentally.

### What would be required
To make input-sync work with RA protocol, NextUI would need to implement **full rollback netcode**:
1. Save emulator state every frame to a ring buffer (~64 slots)
2. Speculatively run frames using predicted (last-known) input
3. When actual remote input arrives for a past frame and differs from prediction, rewind to that frame and replay all frames up to current
4. Compute CRC of game state every frame and exchange via `CMD_CRC`
5. Handle `CMD_LOAD_SAVESTATE` for desync recovery
6. Implement RA's full input command format (frame_num, client_num, per-device input arrays)

### Estimated effort: ~3000-4000 lines, significant rearchitecture of the main loop
- Per-frame `core.serialize()` calls (performance impact on low-power devices)
- Ring buffer of savestates (memory impact: serialize_size * 64)
- Replay loop integration in minarch.c main loop
- CRC computation per frame

### Alternative: Could RA be configured for lockstep?
No. RetroArch does not have a lockstep mode. Rollback is the only synchronization model for input-sync netplay.

### Recommendation
**Defer.** The effort is massive, the performance implications on handheld hardware are significant (per-frame savestates + potential replays), and the use case (cross-platform fbneo/snes9x netplay) is less compelling than Pokemon trading.

---

## 3. GB Link (gblink.c) - Gambatte

**Verdict: ALREADY COMPATIBLE (with caveats)**

### Why it works
Gambatte manages its own TCP connection internally via `HAVE_NETWORK=1` compile flag. Neither NextUI nor RetroArch use their netplay protocols for this - the core handles everything on port 56400.

When NextUI's gambatte connects to RetroArch's gambatte (or vice versa), the TCP connection is between two instances of the same core code. The frontend is irrelevant.

### Caveats
1. **Discovery doesn't cross frontends**: NextUI uses UDP broadcast with magic `GBLC`/`GBLR` on port 56421. RetroArch doesn't use this. Users must enter IP manually.
2. **Both must use the same gambatte build**: The core's internal link protocol must match. If NextUI patches gambatte differently than RetroArch's build, the internal protocol might differ.
3. **Core option names must match**: NextUI sets gambatte core options (`gambatte_gb_link_mode`, `gambatte_gb_link_network_server_ip_*`, etc.) to configure the link. RetroArch uses the same option names since they're defined by the core.

### What to verify
- Test NextUI gambatte host + RetroArch gambatte client with manual IP entry
- Test vice versa
- Confirm the core link protocol is identical across builds

### Estimated effort: 0 lines (maybe documentation/UI for manual IP entry)

---

## Summary

| Link Type | Compatible? | Effort | Priority |
|---|---|---|---|
| **GBA Link** (gpSP) | Can be made compatible | ~800-1000 lines | High - most requested use case |
| **Input-Sync** (fbneo, snes9x, etc.) | Not feasible currently | ~3000-4000 lines + rearchitecture | Low - defer |
| **GB Link** (gambatte) | Already compatible | ~0 lines | N/A - just test & document |

---

## Rollback vs Lockstep Explained

**Lockstep** (what NextUI uses): Both devices must have each other's input **before** advancing a frame. If the remote input hasn't arrived yet, the game **freezes and waits**. Simple, always correct, but any network lag = visible stutter/freezing.

```
Frame 10: Have local input, waiting for remote... (game frozen)
Frame 10: Remote input arrived! Run frame.
Frame 11: Have local input, have remote input. Run frame.
Frame 12: Have local input, waiting for remote... (game frozen)
```

**Rollback** (what RetroArch uses): The game **never waits**. It predicts the remote input (assumes "same as last frame") and runs immediately. If the prediction was wrong when the real input arrives, it **rewinds** to that frame, replays with the correct input, and catches back up. Smoother gameplay, but requires saving game state every frame and the ability to replay frames instantly.

```
Frame 10: Have local input, predict remote input. Run frame speculatively.
Frame 11: Have local input, predict remote. Run frame. Real input for frame 10 arrives - prediction was WRONG!
         -> Rewind to frame 10, replay with correct input, replay frame 11, continue.
Frame 12: Seamless, user saw no stutter.
```

**Why they're incompatible**: If NextUI (lockstep) stalls at frame 10 waiting for input, RetroArch (rollback) has already speculatively run frames 10, 11, 12... They're now on completely different frames with different game states. RetroArch also expects per-frame CRC checks and the ability to send savestate corrections, which NextUI doesn't support.

---

## Recommended Next Steps

If proceeding with GBA Link compatibility:

### Phase 1: RA Protocol Module
- Create `ra_protocol.c/h` implementing RA handshake, command send/recv, NETPACKET wrapping
- Key files: new `workspace/all/netplay/ra_protocol.c`, `workspace/all/netplay/ra_protocol.h`

### Phase 2: GBA Link Integration
- Replace NextUI handshake in `gbalink.c` with RA handshake
- Replace packet format with RA command format
- Key files: `workspace/all/netplay/gbalink.c`, `workspace/all/netplay/gbalink.h`

### Phase 3: Frontend Integration
- Expose core name/version from minarch for RA CMD_INFO
- Update discovery or add manual IP entry for cross-frontend connections
- Key files: `workspace/all/minarch/minarch.c`, `workspace/all/netplay/netplay_helper.c`

### Phase 4: Testing
- NextUI host + RetroArch client (gpSP Pokemon trade/battle)
- RetroArch host + NextUI client (gpSP Pokemon trade/battle)
- NextUI host + NextUI client (regression test)
- GB Link cross-platform test (manual IP)

### Key reference files
- RetroArch source: `network/netplay/netplay_frontend.c` (handshake logic, NETPACKET handling)
- RetroArch source: `network/netplay/netplay_private.h` (command IDs, structures)
- NextUI: `workspace/all/netplay/gbalink.c` (current netpacket bridge)
- NextUI: `workspace/all/netplay/netplay.c` (current input-sync, for reference)
- NextUI: `workspace/all/minarch/minarch.c` (core integration, env callback)
