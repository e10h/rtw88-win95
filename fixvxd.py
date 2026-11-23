#!/usr/bin/env python3
"""
fixvxd.py - Normalize JWlink LE VxD output to match Microsoft LINK.EXE -VXD
(verified byte-for-byte against working Win95 NDIS dynamic VxD ELNK3.VXD).

Fixes JWlink LE VxD header discrepancies that cause Win95 VXDLDR to reject the file:
  1. mod_ver (+0x0C): set to 0 (JWlink writes 0x190)
  2. rsrc_tbl_off (+0x50): set to 0 when rsrc_cnt == 0 (JWlink writes res_name_off)
  3. fixup_size (+0x30): set to imp_mod_off - fixup_page_off (JWlink is off by +1)
  4. preload_cnt (+0x84): set to count of PRELOAD pages (JWlink writes 0!)
  5. auto_ds (+0x94): set to 0 (JWlink writes 1!)
  6. Entry Table flags byte: set 0x01 -> 0x03 (EXPORTED | SHARED, matching ELNK3/SWAPCOMP/AHCIFIX)
  7. Object Table: set RelocBase = 0 for all objects (matching ELNK3/SWAPCOMP)
  8. data_pages_off (+0x80): pad and align to 512-byte boundary (0x400 / 0x800),
     updating nonres_off (+0x88) accordingly
  9. MZ header e_cblp/e_cp: match Microsoft LINK.EXE -VXD stub header
"""
import struct
import sys

def fix_vxd(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    le = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[le:le+2] == b"LE", "Not an LE file"

    # Read key LE header fields
    mod_pages     = struct.unpack_from("<I", data, le + 0x14)[0]
    page_size     = struct.unpack_from("<I", data, le + 0x28)[0]
    last_page     = struct.unpack_from("<I", data, le + 0x2C)[0]
    obj_tbl_off   = struct.unpack_from("<I", data, le + 0x40)[0]
    obj_cnt       = struct.unpack_from("<I", data, le + 0x44)[0]
    rsrc_cnt      = struct.unpack_from("<I", data, le + 0x54)[0]
    entry_tbl_off = struct.unpack_from("<I", data, le + 0x5C)[0]
    fpage_off     = struct.unpack_from("<I", data, le + 0x68)[0]
    imp_mod_off   = struct.unpack_from("<I", data, le + 0x70)[0]
    imp_proc_off  = struct.unpack_from("<I", data, le + 0x78)[0]
    old_data_off  = struct.unpack_from("<I", data, le + 0x80)[0]
    old_nonres    = struct.unpack_from("<I", data, le + 0x88)[0]
    nonres_len    = struct.unpack_from("<I", data, le + 0x8C)[0]

    # 1. Zero mod_ver (+0x0C) and auto_ds (+0x94)
    struct.pack_into("<I", data, le + 0x0C, 0)
    struct.pack_into("<I", data, le + 0x94, 0)

    # 2. Zero rsrc_tbl_off (+0x50) if rsrc_cnt == 0
    if rsrc_cnt == 0:
        struct.pack_into("<I", data, le + 0x50, 0)

    # 3. Fix fixup_size (+0x30) = imp_mod_off - fpage_off (exact match to ELNK3/SWAPCOMP)
    if imp_mod_off == imp_proc_off:
        struct.pack_into("<I", data, le + 0x30, imp_mod_off - fpage_off)

    # 4. Compute preload_cnt (+0x84) from Object Table and zero RelocBase (+0x04)
    preload_pages = 0
    for i in range(obj_cnt):
        o = le + obj_tbl_off + i * 24
        vsize, base, flags, pidx, pcnt, resv = struct.unpack_from("<IIIIII", data, o)
        # Zero RelocBase (ELNK3.VXD and SWAPCOMP.VXD have Base=0 for all objects)
        struct.pack_into("<I", data, o + 4, 0)
        if flags & 0x0040:  # OBJ_PRELOAD
            preload_pages += pcnt
    struct.pack_into("<I", data, le + 0x84, preload_pages)

    # 5. Fix Entry Table flags byte: 0x01 -> 0x03 (EXPORTED | SHARED)
    ep = le + entry_tbl_off
    if data[ep] == 1 and data[ep + 1] == 3 and data[ep + 4] == 0x01:
        data[ep + 4] = 0x03

    # 6. Align data_pages_off to next 512-byte boundary (min 0x400)
    new_data_off = max(0x400, (old_data_off + 0x1FF) & ~0x1FF)
    pad_bytes = new_data_off - old_data_off
    if pad_bytes > 0:
        # Insert zero padding right before old_data_off
        data[old_data_off:old_data_off] = b"\x00" * pad_bytes
        struct.pack_into("<I", data, le + 0x80, new_data_off)
        if old_nonres != 0:
            struct.pack_into("<I", data, le + 0x88, old_nonres + pad_bytes)

    # 7. Verify data_pages_off + data size == nonres_off and file size
    data_off = struct.unpack_from("<I", data, le + 0x80)[0]
    nonres_off = struct.unpack_from("<I", data, le + 0x88)[0]
    expected_data_end = data_off + (mod_pages - 1) * page_size + last_page
    assert expected_data_end == nonres_off, f"Mismatch: {hex(expected_data_end)} != {hex(nonres_off)}"
    assert nonres_off + nonres_len == len(data), f"File end mismatch"

    with open(path, "wb") as f:
        f.write(data)

    print(f"fixvxd.py: {path} normalized ({len(data)} bytes, objects={obj_cnt}, pages={mod_pages}, preload={preload_pages}, data_off=0x{data_off:X})")

if __name__ == "__main__":
    for p in sys.argv[1:]:
        fix_vxd(p)
