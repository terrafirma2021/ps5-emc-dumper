# PS5 13.60 EMC decoder research

## Verified structure

The successful firmware 13.60 ICC response is exactly 2,020 bytes:

- one 4-byte little-endian header;
- 63 records;
- 32 bytes (eight little-endian words) per record.

The native reader saves those bytes without altering them. This structure is
therefore hardware-observed, not inferred from a public NOR decoder.

## Kernel boundary

The 13.60 kernel routine at the currently pinned `ktext + 0xa3f4f0` is used as
an ICC transport. The request selects service `2`, command `7`, with a `0x20`
record size. The reader copies the returned 2,020 bytes verbatim. No error-code
string table or semantic decoding has been found in the kernel path.

## Firmware inspection

The decrypted 13.60 update contains:

- `16_emc_salina_c.bls` — 477,184 bytes, SHA-256
  `2fbe074fcb3d24a1265574f39e99e0a3833d30449a18fe9a825bf23014fd2de5`;
- `20_emc_salina_d.bls` — 477,184 bytes, SHA-256
  `0bfa7266e83f91e46a58f326ebd64c735c1c68ec3ff77ccce065fdddef1d7df6`.

Both are SLB2 v3 containers with two entries. The main entry is named
`C0000001`, begins at block 1, and is 476,112 bytes. Its payload remains a
protected firmware blob; searches for the observed codes as little-endian,
big-endian, and ASCII values found no code table. Consequently these files do
not presently provide an authoritative local label database.

## Community labels

The following public projects contain overlapping reverse-engineered tables:

- <https://github.com/apewalkers/PS5_NOR_Decode>
- <https://github.com/Liams-Electronics-Lab/PS5-Error-Codes->

`tools/decode_emc_log.py` keeps the verified binary layout separate from these
community-derived labels. Unknown codes, masks, power states, and sequence
values remain unknown rather than receiving inferred descriptions.

## Remaining authoritative route

An authoritative decoder requires one of:

1. plaintext EMC firmware obtained through a documented, read-only service or
   update-decryption path;
2. a Sony diagnostic module that formats this exact ICC response;
3. controlled hardware observations that independently validate each field.

Until then, decoded labels must be presented as community-derived repair data,
while the raw 2,020-byte report remains authoritative.
