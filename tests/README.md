# Test bench setup

Recipe to verify the modules against a real Cisco phone without
disrupting an existing chan_sip / chan_pjsip deployment.

## Premise

Run a parallel PJSIP TCP transport on a non-conflicting port (e.g.
`:5160`), point a single test phone at it by editing its SEP file,
and observe the four wire signals — REGISTER 200 OK with optionsind,
post-REGISTER REFER with bulkupdate, unsolicited NOTIFYs per watched
extension, and SUBSCRIBE-response NOTIFYs with Cisco-flavoured PIDF.

Rollback is one SEP-file edit + phone restart.

## Steps

1. **Add a parallel TCP transport.** In `pjsip.conf`:
   ```
   [transport-tcp-test]
   type    = transport
   protocol = tcp
   bind    = <server-ip>:5160
   ```

2. **Add the test endpoint** (parallel to your existing chan_sip
   peer of the same name — they don't conflict because ports differ):
   ```
   [1010]
   type            = endpoint
   transport       = transport-tcp-test
   context         = local_sip_phone
   disallow        = all
   allow           = opus,g722
   dtmf_mode       = rfc4733
   mailboxes       = 1000@default
   auth            = auth-1010
   aors            = 1010

   [auth-1010]
   type            = auth
   auth_type       = userpass
   username        = 1010
   password        = <same as chan_sip secret>

   [1010]
   type            = aor
   max_contacts    = 1
   qualify_frequency = 10
   remove_existing = yes

   [1010]
   type            = cisco
   line_index      = 1
   subscribe       = <comma-list of monitored exts>
   subscribe_context = local_sip_phone
   ```

3. **Reload PJSIP**:
   ```
   asterisk -rx 'module reload res_pjsip.so'
   ```

4. **Reprovision the phone.** Edit the phone's SEP file in your
   TFTP/HTTP root: change `<proxy1_port>5060</proxy1_port>` to
   `<proxy1_port>5160</proxy1_port>`. Power-cycle the phone (pull
   PoE/power for 5 seconds — a soft "Settings → Restart" preserves
   stale subscription state and can confuse the test).

5. **Confirm registration**:
   ```
   asterisk -rx 'pjsip show endpoint 1010'
   # Expect "Not in use" + a Contact entry with Avail status.
   ```

6. **Confirm modules fired** (asterisk log):
   ```
   tail -f /var/log/asterisk/messages | grep cisco-
   # Expect, on each REGISTER:
   #   cisco-optionsind: attached optionsind body to REGISTER 200 OK
   #   cisco-bulkupdate: REFER sent to sip:1010@…
   #   cisco-unsolicited-blf: unsolicited NOTIFY sent for <each ext>
   ```

7. **Inspect the wire** if anything looks wrong:
   ```
   tcpdump -i <iface> -A 'tcp port 5160 and host <phone-ip>'
   ```
   Force a fresh REGISTER for capture by killing the existing TCP:
   ```
   ss --kill state established dst <phone-ip>
   ```

8. **Verify line buttons.** On the phone: each button configured for
   BLF in the SEP file should now render with the **hook icon**, not
   the keypad icon. A monitored extension going off-hook should light
   the corresponding button red.

9. **Test service-control**:
   ```
   asterisk -rx 'pjsip cisco check-sync 1010'
   # Phone refetches TFTP config in the background (no visible reboot).
   asterisk -rx 'pjsip cisco restart 1010'
   # Phone deregisters ("Removed contact ... due to request"), reboots,
   # re-REGISTERs. With `pjsip set logger on`, confirm the NOTIFY body's
   # RegisterCallId matches the phone's REGISTER Call-ID — a mismatch 400s.
   ```
   On a shared line (`max_contacts > 1`), `pjsip cisco restart <ep>`
   should reboot **every** registered phone; `pjsip cisco restart <ep>
   <contact-uri-substring-or-@hash>` should reboot **only** the matching
   one. Both args tab-complete.

10. **Test RemoteCC HLog**. Press the HLog softkey on the phone:
    ```
    asterisk -rx 'database show HuntGroup 1010'
    # Expect /HuntGroup/1010 : YES after the first press, absent after the next.
    ```
    The log should show `cisco-remotecc: 1010 set hunt-group login ...`
    and a follow-up HLog update REFER.

11. **Test RemoteCC MCID** during an active call. Press the MCID softkey
    on the phone. The log should show
    `cisco-remotecc: 1010 queued MCID on ...`, and the wire should show
    a follow-up RemoteCC REFER with statusline and tone feedback. Final
    carrier-side MCID handling depends on the channel/trunk technology.

12. **Test ConfList** during an active call. Press the ConfList softkey
    on the phone. The log should show
    `cisco-conference: 1010 pressed ConfList ...` followed by
    `cisco-conference: ConfList menu sent to ...`, and the phone display
    should show a `Conference` menu listing the other bridge participant
    (or all peers if the call is in a multi-party bridge such as
    `ConfBridge()`). Phase 1 is read-only — Mute / Remove / Update
    softkeys on the menu are not yet wired up.

13. **Rollback.** Edit the SEP `<proxy1_port>` back to `5060`,
    power-cycle. Phone returns to chan_sip; PJSIP test endpoint goes
    Unavailable.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Phone REGISTERs but Contact stays Unavail (NonQual) | Phone behind firewall blocking inbound OPTIONS qualify. Disable qualify temporarily by setting `qualify_frequency = 0` on the AOR, or fix the firewall. |
| Buttons show keypad icon despite all four wire signals firing | Confirm the SEP file actually has `<featureID>21</featureID>` (BLF Speed Dial) for those buttons. Without it, the firmware ignores the server's BLF cues. |
| `cisco-optionsind` log line never appears | Check that the endpoint section is named exactly the same as the cisco section, that pjsip.conf parsed it (`pjsip show endpoint 1010`), and that `[1010] type=cisco` was loaded (`asterisk -rx 'core reload'`). |
| Server crashes shortly after `module unload res_pjsip_pidf_body_generator.so` | Don't hot-unload body generators with active subscriptions — use `systemctl restart asterisk` instead. Our cisco modules guard against this themselves (refuse runtime unload via `ast_shutdown_final`), but stock pidf has no such guard. |
| Startup log shows `WARNING: A body generator for application/cpim-pidf+xml is already registered` | Expected and harmless — that's stock `res_pjsip_xpidf_body_generator` declining after `res_pjsip_cisco_pidf_body_generator` won the slot via earlier `load_pri`. |

## Runtime UB checking (UBSan)

The bench above exercises real call flows; `make sanitize-ub` turns that
same exercise into a memory-correctness check. It rebuilds the modules
with `-fsanitize=undefined`, so undefined behaviour in our code — signed
overflow, bad shifts, null/misaligned deref, out-of-range enum loads,
the runtime side of the conversions `-Wconversion` only catches at
compile time — is reported with a `file:line: runtime error: …` message
while the phone drives traffic. Unlike ASan, UBSan needs no early init,
so it loads into a normally-launched asterisk with no `LD_PRELOAD`.

Run this against the **`:5160` test instance only** — never `make
install` a sanitized `.so` onto a production PBX.

1. **Build the UBSan variant** (same `ASTERISK_SRC_DIR` you build with —
   struct layouts must still match the runtime asterisk):
   ```
   make sanitize-ub ASTERISK_SRC_DIR=/path/to/asterisk-source
   ```
   The `.so` land in `obj/sanitize-ub/`, kept apart from the production
   build so they can't be installed by accident.

2. **Install them over the test asterisk's modules** and note where the
   originals were so you can roll back:
   ```
   MODDIR=$(asterisk -rx 'core show settings' | sed -n 's/.*Module directory:[[:space:]]*//p')
   sudo cp "$MODDIR"/res_pjsip_cisco_*.so /tmp/cisco-so-backup/
   sudo install -m0644 obj/sanitize-ub/res_pjsip_cisco_*.so "$MODDIR"/
   ```

3. **Restart asterisk with UBSan configured.** `halt_on_error=0` keeps
   the PBX alive and logs every hit (use `=1` to abort on the first);
   `print_stacktrace=1` needs `llvm-symbolizer` or `addr2line` on `PATH`
   for symbolised frames:
   ```
   sudo systemctl set-environment \
     UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:log_path=/tmp/ubsan
   sudo systemctl restart asterisk
   ```
   (Or export `UBSAN_OPTIONS` however your asterisk is launched.)

4. **Exercise the modules** — run through the bench steps above
   (register the phone, press the softkeys, place calls) and/or
   `bash tests/sipp/run.sh`.

5. **Read the reports:**
   ```
   cat /tmp/ubsan.*
   # Each entry: <our-file>:<line>: runtime error: <description>
   ```
   No files / empty output = no UB tripped on the paths you exercised.

6. **Roll back** when done — reinstall the production `.so` and restart:
   ```
   sudo install -m0644 /tmp/cisco-so-backup/res_pjsip_cisco_*.so "$MODDIR"/
   sudo systemctl unset-environment UBSAN_OPTIONS
   sudo systemctl restart asterisk
   ```

ASan (heap/use-after-free) would need `LD_PRELOAD=libasan.so` plus
`ASAN_OPTIONS=detect_leaks=0` and isn't wired up here; MSan is
impractical for a module loaded into a non-instrumented asterisk.

## What "working" looks like — wire signature

For a fresh power-cycle of a Cisco 7975 against PJSIP with all ten
modules loaded and a `[name] type=cisco` section configured:

```
192.0.2.x.<rand>  -> 198.51.100.x.5160  REGISTER
198.51.100.x.5160     -> 192.0.2.x.<rand> 401 (auth challenge)
192.0.2.x.<rand>  -> 198.51.100.x.5160  REGISTER + Authorization
198.51.100.x.5160     -> 192.0.2.x.<rand> 200 OK
                        Content-Type: application/x-cisco-remotecc-response+xml
                        <body: optionsind …>
198.51.100.x.5160     -> 192.0.2.x.<rand> REFER
                        Content-Type: multipart/mixed; boundary=uniqueBoundary
                        <body: dndupdate / hlogupdate / bulkupdate …>
192.0.2.x.<rand>  -> 198.51.100.x.5160  202 Accepted (REFER)
192.0.2.x.<rand>  -> 198.51.100.x.5160  SUBSCRIBE  Event: presence  Resource: 1001
198.51.100.x.5160     -> 192.0.2.x.<rand> 200 OK
198.51.100.x.5160     -> 192.0.2.x.<rand> NOTIFY  Event: presence  (in-dialog response)
                        Content-Type: application/cpim-pidf+xml
                        <body: Cisco-flavoured PIDF …>
198.51.100.x.5160     -> 192.0.2.x.<rand> NOTIFY  Event: presence  (UNSOLICITED, from server)
                        Content-Type: application/pidf+xml
                        <body: same Cisco PIDF, From: <sip:1001@…>>
…
```

The unsolicited NOTIFYs (last lines) are the trigger that flips the
buttons into BLF mode — the in-dialog response NOTIFYs alone are not
enough.

During a normal call to or from the phone, INVITE/UPDATE traffic for
the Cisco endpoint should also include `Supported: X-cisco-sis-10.0.0`
and `Call-Info: <urn:x-cisco-remotecc:callinfo>;orientation=...`. If
`CISCO_CALLBACK_NUMBER` is set on the channel and the endpoint sends
Remote-Party-ID, the RPID URI should include
`x-cisco-callback-number=...`. H.264 video offers should include
`b=TIAS:4000000` and `a=imageattr:<pt> recv [x=640,y=480,q=0.50]`
when no imageattr is already present.
