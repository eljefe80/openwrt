# ASRock X10 IR-blaster MCU: I2C protocol recovered from stock /sbin/test_ir

The source is `rootfs/sbin/test_ir`: ARM32, unoptimised gcc with fp frames, 10682 bytes, text 0x8898-0xa2b0.
The full annotated disassembly is in `ir-re/dis.txt` and the generator is `ir-re/dis.py`.
Tags:
- **[V @addr]** means VERIFIED-FROM-DISASSEMBLY at that function or instruction address.
- **[I]** means INFERRED.

## Function map
| addr | role |
|---|---|
| 0x8968 | main: arg dispatch |
| 0x8fdc | frame_init(a,b,c): clears 4 bytes of each buffer, then sets b[0..2] = FF 61 00 |
| 0x9058 | build_frame(cmd, len, data, frame) |
| 0x91b0 | extract_payload(out, rx): prints "the data is blank" when the payload is empty |
| 0x927c | cmd_led |
| 0x934c | cmd_ver |
| 0x9408 | cmd_upgrade |
| 0x9738 | build_fw_block(block256, frame, word_addr) |
| 0x989c | cmd_learn |
| 0x9990 | cmd_send |
| 0x9a44 | i2c_write(fd, n, buf): one write() call, resets the MCU and retries once on failure |
| 0x9b0c | i2c_read_resp(fd, buf, n): ACK read, then data read |
| 0x9bcc | read_status2(fd, buf, s1, s2) |
| 0x9ca8 | check_in_high: GPIO60 wait |
| 0x9e5c | save /tmp/irdatax |
| 0x9f58 | load /tmp/irdatax |
| 0xa110 | debug dump of the learned frame |
| 0xa22c | reset (GPIO46) |
| 0xa28c | usage |

## 1. Bus and address [V @0x89cc-0x8a28]
- The tool opens `/dev/i2c-0`: `snprintf("/dev/i2c-%d", 0)`, then `open(O_RDWR)`.
  - [I] On a new OpenWrt kernel, check which index the i2c-gpio bus (SDA 59, SCL 58) actually gets.
- It makes one ioctl, `ioctl(fd, I2C_SLAVE=0x0703, 0x50)`. The 7-bit slave address is **0x50**. It uses no other address and no other ioctl. It does not use I2C_RDWR or SMBus.
- Every transfer is a plain `write(fd, buf, n)` or `read(fd, buf, n)`. Each call is one complete I2C transaction (START, 0x50 with R or W, n bytes, STOP). There is no register address phase and no repeated start.

## 2. Frame format

### Host to MCU [V @0x9058]
```
off: 0    1    2    3      4      5    6 .. 6+N-1   6+N   7+N
     FF   61   00   LenLo  LenHi  CMD  DATA[N]      CS    F0
Len = N + 2 (16-bit little-endian)
CS  = low 8 bits of the sum of bytes [2 .. 5+N] (00, LenLo, LenHi, CMD, DATA...)
Total length written = N + 8
```
- Bytes 0-2 (FF 61 00) come from frame_init [V @0x9024-0x904c].
- The checksum is accumulated in 16 bits, and only the low byte is stored [V @0x9124-0x9188].
- The trailer is a constant 0xF0 (`mvn #0xf`) [V @0x919c].

### MCU to host responses (what the tool parses)
- **ACK/status frame:** the tool reads 8 bytes and checks only byte [5]. It does not validate the header, length or checksum [V @0x9b3c, 0x9c10].
  - 0x7F means ACK/OK.
  - 0x81 and 0x82 are second-stage completion codes (see the send and upgrade commands).
  - [I] By symmetry, the ACK is probably `FF 61 00 02 00 <code> CS F0`.
- **Data frame:**
  - Payload length = `(rx[4]<<8 | rx[3]) - 2`.
  - The payload starts at rx[6] [V @0x91cc-0x9250].
  - A length of 0 prints "Error: the data is blank, maybe time out, please try again!!" and fails.
  - The tool does not check the header, CS or F0.

### Read primitive, i2c_read_resp(fd, buf, n) [V @0x9b0c]
1. Call check_in_high().
2. `read(8)`. If `buf[5] != 0x7F`, print "format error" and fail.
3. Call check_in_high().
4. `memset(buf, 0, 4)`.
5. `read(n)`. This is the data frame, which overwrites the buffer.

### Status primitive, read_status2(fd, buf, s1, s2) [V @0x9bcc]
1. check_in_high(), then `read(8)`. Byte [5] must equal s1, otherwise "status1 error".
2. check_in_high(), then `read(8)`. Byte [5] must equal s2, otherwise "status2 error".

### Write primitive [V @0x9a44]
- `write(fd, frame, N+8)`.
- If write() returns a negative value, the tool runs reset (0xa22c, GPIO46 pulse) and calls write() **once** more.
- It reports failure only if the second write also fails.

## 3. GPIO handshake, check_in_high [V @0x9ca8]
- **GPIO number:** 60 (`mov r3,#0x3c`).
- **Setup:**
  1. `sprintf(p, "sys/class/gpio/gpio%d/direction", 60)` and `access(p, F_OK)`.
  2. If that fails, write "60" to `/sys/class/gpio/export`.
  3. Open the same direction path with O_WRONLY and write "in".
- **Poll loop:**
  1. Open `/sys/class/gpio/gpio60/value`, read 1 byte, close.
  2. Repeat while the byte is `'0'`.
  3. It therefore **waits until GPIO60 reads high** (any character other than '0').
- **No timeout and no sleep:** this is a tight busy-loop. If the line never rises, the tool hangs forever. The caller's `flock /var/irlock` keeps other callers blocked during that time.
- **Stock bug [V]:** the direction path has **no leading '/'** (string at 0xa4cc: `"sys/class/gpio/gpio%d/direction"`).
  - With cwd=`/`, everything works.
  - With any other cwd, access() fails and "60" is written to export (harmless).
  - The relative open(O_WRONLY) then fails. The tool prints `check_in_high error 427` (0x1ab) and **returns without polling at all**.
  - So in some contexts the stock tool ran with no handshake (for example from CGI, depending on cwd).
- [I] Semantics: GPIO60 is an MCU-driven "response ready / not busy" line, active high. A clean-room tool should:
  - export GPIO60 as an input properly;
  - poll it with a short sleep (for example 1 ms) and a timeout;
  - use a long timeout for -learn, because the MCU holds the line low until it captures a code or times out on its own (it returns an empty frame then).

## 4. Commands

Main [V @0x8968]:
- The tool walks argv from index 1.
- `-d` sets a global debug flag at 0x129c8, which enables verbose printf output.
- An unknown argument prints "argv=%s", then the usage text, and returns -1.
- `-help` prints usage and returns 0.
- After a command has run, main **returns 0 whatever the result**. Handler return values are ignored. The only nonzero exit is -1, for bad arguments or an open/ioctl failure.

### -ver [V @0x934c]
1. Send cmd **0x10**. N = argc-2, which is 0 for a plain `ir_cmd -ver`.
   - Frame: `FF 61 00 02 00 10 12 F0`.
2. Call i2c_read_resp with n = **24**: wait, read 8 (ACK 0x7F), wait, read 24.
3. Extract the payload and print `printf("IR FW version = %s\n", payload)`.
   - The output buffer is not explicitly NUL-terminated beyond its first 4 bytes.
- [I] The payload is ASCII `AR_v1.23 20170607`. That string sits at offset 0x1fbc of the .SN8 file. It fits: 6 + 17 = 23 bytes, inside the 24-byte read. ir_auto_upgrade's sed/awk parsing (`v1` . `23`, fields $5 $6) matches it.
- Recommendation: read more than 24 bytes and terminate the string at the payload length.

### -led b0 b1 b2 ... [V @0x8b64, 0x927c]
- **Argument parsing:** every remaining argv is parsed with `strtol(arg, 0, 16)` and truncated to a byte. Both "FF" and "0xFF" work. N = argc-2.
  - Stock quirk: with `-d`, N is still argc-2, so the tool sends one extra 0x00 byte.
- Send cmd **0x12** with the N raw bytes. Then check_in_high, `read(8)`, and byte [5] must be 0x7F, otherwise "led set fail".
  - Only one GPIO wait happens here. The tool does not use i2c_read_resp.
- Examples:
  - `-led 0 FF FF` → `FF 61 00 05 00 12 00 FF FF 15 F0`
  - `-led 0 0 0` → `... 12 00 00 00 17 F0`
  - `-led 0 FF 0` → `... 12 00 FF 00 16 F0`
- [I] Byte meanings, from x10_leds_service: b0 is always 00 (LED index or mode), b1 is red (00/ff), b2 is blue brightness (`$lighting_level`). The MCU firmware defines the real semantics; the tool forwards the bytes untouched.

### -learn [V @0x8d10, 0x989c]
1. `system("rm /tmp/irdatax > /dev/null 2>&1")`.
2. Send cmd **0x30** with N = argc-2, which is 0: `FF 61 00 02 00 30 32 F0`.
3. Call i2c_read_resp with n = **512 (0x200)**: wait, ACK 0x7F, wait (this is the long wait while the user aims the remote), read 512.
4. Print "IR low data = [%d]", where %d = rx len field + 6, which is the total frame size [V @0xa110].
5. Extract the payload; an empty payload means "data is blank, maybe time out".
6. Write `/tmp/irdatax` [V @0x9e5c]:
   - `fopen("w+")`.
   - Each byte as `"%02X "`, uppercase hex followed by a space, except the last byte, which is written as `"%02X"`.
   - There is no newline.
   - The payload is stored exactly as received: no header, no CS.
- **Maximum code length:** the read is 512 bytes, so the payload can be at most 512-8 = **504 bytes**. The stock tool does not bound-check the length field against the read size. Its stack buffer is 512 bytes.

### -send [V @0x8db0, 0x9990, 0x9f58]
- The tool ignores CLI arguments and always reads `/tmp/irdatax`:
  - `fopen("r")`, then a `fgets` loop over lines of up to 1000 bytes;
  - each line is split with `strtok(" ")`;
  - each token is parsed with `strtoul(tok, 0, 16)` into the next byte;
  - lines are concatenated, and there is no bounds check (the buffer is 512 bytes).
- Prints `********Open IR Data = [%d] bytes********`.
- If the count is 0, the command fails silently.
- Send cmd **0x02** with the N file bytes; total write = N+8.
- Then read_status2 with s1 = **0x7F** and s2 = **0x81**: wait, read 8, check for 0x7F; wait, read 8, check for 0x81. [I] 0x81 means "transmission finished".
- Round-trip: the bytes that -send transmits are exactly the payload that -learn received.
- [I] The payload contents (carrier and pulse timing encoding) are opaque, defined by the MCU firmware. The tool never interprets them.

### -reset [V @0xa22c]
The tool runs these `system()` calls, in order:
```
echo 46 > /sys/class/gpio/export
echo "out" > /sys/class/gpio/gpio46/direction
echo "0" > /sys/class/gpio/gpio46/value
usleep 200000
echo "1" > /sys/class/gpio/gpio46/value
usleep 200000
echo "in" > /sys/class/gpio/gpio46/direction
```
- No I2C traffic is involved.
- Effect: GPIO46 is driven low for 200 ms, then high for 200 ms, then released to input.
- [I] GPIO46 is the MCU's active-low reset.
- The write primitive also calls this reset automatically once, on a write() error.

### -upgrade <path> [V @0x8e38, 0x9408, 0x9738]
- The path is the next argv (strcpy into an 0x200 buffer). The tool prints "FW Path = %s", then `fopen(path, "rb")`, then "### Upgrade Start".
- **Image region:** file offsets **0x100 to 0x40FF**, which is 64 blocks of 256 bytes.
  - Offsets 0x00-0xFF are an ASCII header ("SN8F27E93L ... 00004000 ...") and are skipped.
  - Offsets 0x4100-0x80FF exist in the file but are **not sent**. That region is 99.95% zero.
- **For block k = 1..64,** where file_off = 0x100·k and word_addr = 0x80·(k-1):
  1. fseek to file_off and fread 256 bytes.
  2. Add all 256 bytes into a running **16-bit sum over the whole image**. The sum is never reset between blocks.
  3. Build the frame with build_fw_block [V @0x9738]:
     ```
     FF 61 00 04 01 0E AddrLo AddrHi DATA[256] CS F0   (266 = 0x10A bytes)
     Len = 0x0104 (258 + 2), CMD = 0x0E, Addr = word_addr LE (0x0000, 0x0080, ... 0x1F80)
     CS = low byte of sum(frame[2..0x107])
     ```
  4. Write 266 bytes. If that fails, abort with return 1.
  5. Delay: **sleep(4)** after block 1 ([I] flash erase), and **usleep(50000)** after every other block.
  6. Run frame_init, then read_status2 with s1 = **0x7F** and s2 = **0x82**. Abort on mismatch.
  7. Debug mode prints "fw upgrade %d/%d" with k and 64.
- **Final step:**
  1. The data is the 2-byte image sum, little-endian. Cmd **0x0F**, N = 2: `FF 61 00 04 00 0F SumLo SumHi CS F0` (10 bytes).
  2. usleep(50000).
  3. read_status2 with 0x7F and 0x82.
  4. Print "### burn close". ("final check" appears only with -d.)
- For AR_v1.23_20170607.SN8, the computed image sum is **0xEDD6**. The final frame would be `FF 61 00 04 00 0F D6 ED D6 F0`.
- [I] The stock tool sends no explicit "enter bootloader" command. The MCU firmware presumably accepts cmd 0x0E or 0x0F in normal mode (self-programming, an ISP-capable SN8F27E93L).
- Whether a failed upgrade bricks the MCU, and whether -reset recovers it, is **unknown**.

## 5. Command and status code summary
| code | direction | meaning |
|---|---|---|
| 0x02 | host to MCU | send IR (payload from /tmp/irdatax) |
| 0x0E | host to MCU | firmware block (2-byte word address + 256 bytes) |
| 0x0F | host to MCU | firmware finish (2-byte LE sum) |
| 0x10 | host to MCU | get version |
| 0x12 | host to MCU | LED set |
| 0x30 | host to MCU | learn |
| 0x7F | MCU to host, resp[5] | ACK |
| 0x81 | MCU to host, resp[5] | send complete (2nd status) |
| 0x82 | MCU to host, resp[5] | firmware block or final complete (2nd status) |

## 6. Open items and ambiguities
- The MCU's handling of extra or short LED bytes, and what b0 means [I].
- The exact bytes of the ACK frame other than byte [5]. The tool ignores them; capture them on hardware.
- The kernel bus index on OpenWrt, compared with stock `/dev/i2c-0`.
- check_in_high's real timing (how long GPIO60 stays low per command) has not been measured. The stock tool has no timeout and may skip the wait entirely depending on cwd (see §3).
