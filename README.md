# PS5 EMC Dumper

Native PS5 application for reading and saving the persistent EMC error log on
**firmware 13.60**. The title ID is `PPSA99760`.

The application boots into a native OpenGL interface. It does not touch the EMC
reader until **X** is pressed. **Circle** exits cleanly to the PS5 home screen.

The release is a self-contained installable title, but it has a runtime
dependency on an already-running `elfldr` service. No PC-side transfer is
needed after installation: the title sends its bundled reader internally over
the PS5's existing `127.0.0.1:9021` loopback connection. It does not create the
loopback interface or start `elfldr` itself.

## How it works

The project has two deliberately separate parts:

1. `PPSA99760/eboot.bin` is the native PS5 user application. It draws the UI,
   reads the controller, enforces one attempt per app session, and displays the
   final status. Merely opening the title does not issue an EMC command.
2. When X is pressed, the application sends its bundled `emc-reader.elf` to the
   already-running local `elfldr` service at `127.0.0.1:9021`. This transfer
   remains entirely inside the PS5; it is not sent to a PC or another network
   device. The firmware-specific reader performs one ICC service-2, command-7
   request and restores its temporary hooks and mappings before reporting
   success.

The response is exactly 2,020 bytes: a four-byte little-endian header followed
by 63 records of eight little-endian 32-bit words. The reader preserves those
bytes unchanged, writes machine-readable and text reports, and mirrors a short
summary back into the title directory for the native UI.

Transport and file-layout behavior has been verified on firmware 13.60. Sony's
complete semantic error-code table is not public, so descriptive labels are
kept separate from the raw data and identified as community-derived. Unknown
codes and masks remain unknown rather than being guessed.

> [!WARNING]
> This is experimental firmware-specific software that temporarily executes
> kernel code. It is for PS5 13.60 only. Use one read per clean boot. Reboot the
> console before another attempt. Do not load unrelated standalone kstuff over
> the matching etaHEN environment.

## What it produces

After a successful read, the PS5 contains:

```text
/data/emc-error-log/latest.bin     authoritative 2,020-byte response
/data/emc-error-log/latest.txt     raw records as little-endian words
/data/emc-error-log/latest.json    structure and counts
/data/emc-error-log/decoded.txt    cautious human-readable report
/data/emc-error-log/status.json    completion or failure status
```

The native UI also displays the grouped error-code summary. Public descriptions
are community-derived and are labelled accordingly; `latest.bin` remains the
authoritative artifact.

## Requirements

- PS5 firmware **13.60**
- Byepervisor Reborn active
- A matching 13.60 etaHEN/kstuff stack
- ShadowMountPlus or an equivalent folder-title loader
- `elfldr` listening on TCP port 9021
- FTP or another method to copy the release folder to the console

The tested order is:

1. ReLapse / Payload Manager
2. Byepervisor Reborn
3. matching etaHEN 11.00–13.60 bundle
4. matching ShadowMountPlus build
5. PS5 EMC Dumper

## Install the release

1. Download `PS5-EMC-Dumper-PPSA99760-v0.1.0.zip` and verify it with
   `SHA256SUMS.txt`.
2. Extract the `PPSA99760` folder.
3. Copy the complete folder to:

   ```text
   /data/homebrew/PPSA99760
   ```

4. Add this exact line to `/data/shadowmount/manual.lst`:

   ```text
   /data/homebrew/PPSA99760
   ```

5. Start or rescan ShadowMountPlus and wait for `PS5 EMC Dumper` to appear on
   the home screen.

## Run it

1. Start **PS5 EMC Dumper** from the PS5 home screen.
2. Confirm the native UI says `READY`.
3. Press **X once**.
4. Wait for `EMC QUERY COMPLETE`.
5. Review the grouped codes on screen or retrieve the files from
   `/data/emc-error-log/`.
6. Press **Circle** to exit.
7. Reboot before attempting another EMC read.

If the app reports a failure, do not press X again or relaunch it to retry in
the same boot. Reboot and restore the known-good payload chain first.

The reader restores its temporary entry hooks and mappings before publishing a
successful report. If it cannot prove safe cleanup, it retains its process and
reports a failure where possible rather than claiming success.

## Offline decoding

```bash
python tools/decode_emc_log.py latest.bin \
  --json decoded.json --text decoded.txt
```

The offline decoder rejects files that are not exactly 2,020 bytes. Its field
layout is hardware-verified; descriptive labels are intentionally conservative.

## Building

The build currently uses the same PS5 OpenGL native-title toolchain and
`ps5-payload-sdk` layout used during hardware validation.

```bash
cd reader
make standalone

cd ../app
./native/build-native.sh
```

The build script accepts:

- `PS5_OPENGL_SOURCE`
- `PS5_NATIVE_APP_TEMPLATE`
- `PS5_PAYLOAD_SDK`
- `PS5_OPENGL_PREFIX`
- `PS5_CLANG`

Output is written to `app/dist/PPSA99760`.

## Repository layout

```text
app/native/       native title UI and build script
reader/           firmware-specific one-shot EMC reader
tools/            strict offline decoder
docs/             format and firmware research notes
```

## Credits and data provenance

- Native title uses the PS5 OpenGL and Dear ImGui toolchain.
- Payload build uses ps5-payload-sdk and firmware-specific support code.
- Community error labels are cross-referenced from:
  - <https://github.com/apewalkers/PS5_NOR_Decode>
  - <https://github.com/Liams-Electronics-Lab/PS5-Error-Codes->

See [`docs/DECODER-RESEARCH.md`](docs/DECODER-RESEARCH.md) for the distinction
between verified structure and community-derived semantics.

## License

Project-specific source is provided under the MIT License. External SDK,
OpenGL, ImGui, and community reference material retain their own licenses.
