#!/usr/bin/env python3
"""
Extract USB descriptors from a built ELF (RISC-V / MounRiver) and validate the
UAC2 + CDC composite descriptor set against USB Audio 2.0 and USB 2.0 rules.

Usage:  python audit_uac2.py <path-to-elf> [symbol ...]
Default symbols: device_desc, conf_desc, g_audio_fs_table

It reads the *compiled* bytes, so it catches real macro-expansion defects
(declared bLength vs. bytes actually emitted), not just source intent.

Field offsets used below (USB Audio 2.0 spec):
  AC header      : bcdADC[3:5] bCategory[5] wTotalLength[6:8] bmControls[8]
  Clock Source   : bClockID[3] bmAttributes[4] bmControls[5]
  Input Terminal : bTerminalID[3] wTerminalType[4:6] bAssocTerminal[6]
                   bCSourceID[7] bNrChannels[8] bmChannelConfig[9:13]
                   iChannelNames[13] bmControls[14:16] iTerminal[16]
  Output Terminal: bTerminalID[3] wTerminalType[4:6] bAssocTerminal[6]
                   bSourceID[7] bCSourceID[8] bmControls[9:11] iTerminal[11]
  Feature Unit   : bUnitID[3] bSourceID[4] bmaControls[5..]
  AS General     : bTerminalLink[3] bmControls[4] bFormatType[5]
                   bmFormats[6:10] bNrChannels[10] bmChannelConfig[11:15]
                   iChannelNames[15]
"""
import struct
import sys

# ---------------------------------------------------------------- ELF reader


def read_elf(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF":
        raise SystemExit("not an ELF file: %s" % path)
    if d[4] != 1:
        raise SystemExit("only ELF32 supported")
    endian = "<" if d[5] == 1 else ">"
    (e_type,) = struct.unpack_from(endian + "H", d, 0x10)
    (e_shoff,) = struct.unpack_from(endian + "I", d, 0x20)
    (e_shentsize,) = struct.unpack_from(endian + "H", d, 0x2E)
    (e_shnum,) = struct.unpack_from(endian + "H", d, 0x30)
    (e_shstrndx,) = struct.unpack_from(endian + "H", d, 0x32)

    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from(endian + "10I", d, off)
        secs.append(dict(name_off=name, type=typ, addr=addr, offset=offset,
                         size=size, link=link, entsize=entsize))

    shstr = secs[e_shstrndx]

    def cstr(base, o):
        e = d.index(b"\x00", base + o)
        return d[base + o:e].decode("utf-8", "replace")

    for s in secs:
        s["name"] = cstr(shstr["offset"], s["name_off"])

    syms = []
    for s in secs:
        if s["type"] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s["link"]]["offset"]
        for i in range(s["size"] // 16):
            o = s["offset"] + i * 16
            nm, val, sz, info, other, shndx = struct.unpack_from(
                endian + "IIIBBH", d, o)
            nm = cstr(strtab, nm)
            if nm:
                syms.append(dict(name=nm, value=val, size=sz, shndx=shndx))
    return dict(data=d, secs=secs, syms=syms, e_type=e_type)


def bytes_of(elf, secs, sym):
    """Return the bytes of a symbol, working for both ET_REL (.o) and ET_EXEC."""
    data = elf["data"]
    idx = sym["shndx"]
    if idx == 0 or idx >= len(secs):
        raise SystemExit("symbol %s has no section (shndx=%d)" % (sym["name"], idx))
    sec = secs[idx]
    if elf["e_type"] == 1:  # ET_REL: st_value is section-relative
        start = sec["offset"] + sym["value"]
    else:                   # ET_EXEC: st_value is a virtual address
        start = sec["offset"] + (sym["value"] - sec["addr"])
    return data[start:start + sym["size"]]


# ------------------------------------------------------------- descriptor db

AC_SUBTYPE = {
    1: "HEADER", 2: "INPUT_TERMINAL", 3: "OUTPUT_TERMINAL",
    4: "MIXER_UNIT", 5: "SELECTOR_UNIT", 6: "FEATURE_UNIT",
    7: "PROCESSING_UNIT", 8: "EXTENSION_UNIT", 0x0A: "CLOCK_SOURCE",
    0x0B: "CLOCK_SELECTOR", 0x0C: "CLOCK_MULTIPLIER",
    0x0D: "SAMPLE_RATE_CONVERTER",
}
AS_SUBTYPE = {1: "AS_GENERAL", 2: "FORMAT_TYPE", 3: "ENCODER"}
EP_TYPE = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}


def hexs(b):
    return " ".join("%02X" % x for x in b)


class Audit:
    def __init__(self):
        self.errors, self.warns, self.info = [], [], []

    def err(self, m):
        self.errors.append(m)

    def warn(self, m):
        self.warns.append(m)

    def note(self, m):
        self.info.append(m)


def check_config(conf, a):
    total = len(conf)
    if conf[0] != 9 or conf[1] != 2:
        a.err("config descriptor header invalid: %s" % hexs(conf[:2]))
    wTotal = struct.unpack_from("<H", conf, 2)[0]
    nintf = conf[4]
    if wTotal != total:
        a.err("wTotalLength=%d but descriptor array is %d bytes" % (wTotal, total))
    a.note("config: wTotalLength=%d (array=%d), bNumInterfaces=%d, "
           "bmAttributes=0x%02X, bMaxPower=%d mA"
           % (wTotal, total, nintf, conf[7], conf[8] * 2))

    interfaces = {}
    iads = []
    entities = {}
    ac_if = None
    ac_header_off = None
    ac_header_len = None
    as_list = []
    cur = None
    p = conf[0]
    while p + 1 < total:
        blen, btype = conf[p], conf[p + 1]
        if blen == 0:
            a.err("zero-length descriptor at offset %d" % p)
            break
        if p + blen > total:
            a.err("descriptor at offset %d (type 0x%02X) overruns config: "
                  "len=%d remaining=%d" % (p, btype, blen, total - p))
            break
        body = conf[p:p + blen]
        key = None
        if cur is not None:
            key = cur["key"]

        if btype == 4:  # INTERFACE
            ifnum, alt, nep = body[2], body[3], body[4]
            cls, sub, proto = body[5], body[6], body[7]
            cur = dict(key=(ifnum, alt), off=p, nep=nep, cls=cls, sub=sub,
                       proto=proto, endpoints=[])
            interfaces[(ifnum, alt)] = cur
            a.note("IF %d alt %d: class=0x%02X sub=0x%02X proto=0x%02X nEp=%d"
                   % (ifnum, alt, cls, sub, proto, nep))
            if cls == 1 and sub == 2 and proto != 0x20:
                a.err("IF %d alt %d is AudioStreaming but bInterfaceProtocol="
                      "0x%02X (UAC2 requires 0x20)" % (ifnum, alt, proto))
            if cls == 1 and sub == 1:
                if ac_if is not None:
                    a.err("second AudioControl interface (%d and %d)" % (ac_if, ifnum))
                ac_if = ifnum
        elif btype == 5:  # ENDPOINT
            addr, attr = body[2], body[3]
            mps = struct.unpack_from("<H", body, 4)[0]
            ival = body[6]
            if cur is None:
                a.err("endpoint 0x%02X outside any interface" % addr)
            else:
                cur["endpoints"].append(dict(addr=addr, attr=attr, mps=mps,
                                             ival=ival, off=p, len=blen))
                if len(cur["endpoints"]) > cur["nep"]:
                    a.err("IF %d alt %d: more endpoint descriptors than "
                          "bNumEndpoints=%d" % (cur["key"][0], cur["key"][1], cur["nep"]))
            a.note("  EP 0x%02X attr=0x%02X type=%s sync=%d mps=%d bInterval=%d"
                   % (addr, attr, EP_TYPE[attr & 3], (attr >> 2) & 3, mps, ival))
            if blen != 7:
                a.err("endpoint descriptor bLength=%d (7 expected)" % blen)
            if (attr & 3) == 1:
                if mps > 1024:
                    a.err("EP 0x%02X isochronous wMaxPacketSize=%d > 1024 (HS limit)"
                          % (addr, mps))
                if mps == 0:
                    a.err("EP 0x%02X isochronous wMaxPacketSize=0" % addr)
        elif btype == 0x0B:  # IAD
            iads.append((p, body))
            a.note("IAD: firstIf=%d count=%d class=0x%02X sub=0x%02X proto=0x%02X"
                   % (body[2], body[3], body[4], body[5], body[6]))
            if body[0] != 8:
                a.err("IAD bLength=%d (8 expected)" % body[0])
        elif btype == 0x24 and (cur is not None and cur["cls"] == 1):
            # class-specific interface inside an Audio function
            st = body[2]
            if cur is None:
                a.err("CS_INTERFACE at offset %d outside an interface" % p)
            elif cur["sub"] == 1:  # AudioControl
                name = AC_SUBTYPE.get(st, "SUBTYPE_%d" % st)
                a.note("  AC %s (len=%d): %s" % (name, body[0], hexs(body)))
                if st == 1:
                    if body[0] != 9:
                        a.err("AC header bLength=%d (9 expected)" % body[0])
                    bcd = struct.unpack_from("<H", body, 3)[0]
                    if bcd != 0x0200:
                        a.err("AC header bcdADC=0x%04X (UAC2 requires 0x0200)" % bcd)
                    ac_header_off = p
                    ac_header_len = struct.unpack_from("<H", body, 6)[0]
                    a.note("    bcdADC=0x%04X bCategory=0x%02X wTotalLength=%d "
                           "bmControls=0x%02X" % (bcd, body[5], ac_header_len, body[8]))
                    if p + ac_header_len > total:
                        a.err("AC header wTotalLength=%d overruns config" % ac_header_len)
                elif st in (2, 3, 4, 5, 6, 7, 8, 0x0A, 0x0B, 0x0C, 0x0D):
                    eid = body[3]
                    if eid in entities:
                        a.err("duplicate entity id %d (%s and %s)"
                              % (eid, entities[eid][2], name))
                    entities[eid] = (st, body, name)
                    a.note("    entity id=%d %s len=%d" % (eid, name, body[0]))
            elif cur["sub"] == 2:  # AudioStreaming
                name = AS_SUBTYPE.get(st, "SUBTYPE_%d" % st)
                a.note("  AS %s (len=%d): %s" % (name, body[0], hexs(body)))
                if st == 1:
                    as_list.append(dict(ifnum=cur["key"][0], alt=cur["key"][1],
                                        desc=body, iface=cur))
                    if body[0] != 16:
                        a.err("AS General bLength=%d (16 expected)" % body[0])
                elif st == 2:
                    if body[0] != 6:
                        a.err("FormatType bLength=%d (6 expected)" % body[0])
                    if body[3] != 1:
                        a.err("bFormatType=%d (TYPE_I=1 expected)" % body[3])
                    if body[4] not in (1, 2, 3, 4):
                        a.err("bSubslotSize=%d (1..4)" % body[4])
                    if body[5] > 8 * body[4]:
                        a.err("bBitResolution=%d exceeds 8*bSubslotSize=%d"
                              % (body[5], 8 * body[4]))
                    if as_list:
                        as_list[-1]["format"] = body
        elif btype == 0x24:  # class-specific interface of a non-audio class
            a.note("  CS_INTERFACE (class 0x%02X sub 0x%02X): %s"
                   % (cur["cls"] if cur else -1, cur["sub"] if cur else -1, hexs(body)))
        elif btype == 0x25:  # class-specific endpoint
            a.note("  CS_ENDPOINT (len=%d): %s" % (body[0], hexs(body)))
            if body[0] != 8:
                a.err("UAC2 CS endpoint descriptor bLength=%d (8 expected)" % body[0])
        p += blen

    # ---- AC header wTotalLength consistency
    if ac_header_off is not None:
        end = ac_header_off + ac_header_len
        if end <= total and conf[end] != 0x09:
            a.err("AC wTotalLength=%d ends at offset %d, but the next descriptor "
                  "there is %s (not an interface)" % (ac_header_len, end,
                                                      hexs(conf[end:end + 2])))
        elif end <= total:
            a.note("AC region = offsets %d..%d (interface %d), next descriptor "
                   "type=0x%02X" % (ac_header_off, end - 1, conf[end] - 0,
                                    conf[end + 1]))

    # ---- interface numbering
    nums = sorted(set(k[0] for k in interfaces))
    if nums != list(range(nintf)):
        a.err("bNumInterfaces=%d but interface numbers found: %s" % (nintf, nums))
    for n in nums:
        alts = sorted(k[1] for k in interfaces if k[0] == n)
        if alts != list(range(len(alts))):
            a.err("interface %d alternate settings are %s (must be 0..N-1)" % (n, alts))

    for p, iad in iads:
        first, cnt = iad[2], iad[3]
        if cnt < 2:
            a.err("IAD declares bInterfaceCount=%d (<2)" % cnt)
        for x in range(first, first + cnt):
            if (x, 0) not in interfaces:
                a.err("IAD covers interface %d which does not exist" % x)

    # ---- entity / topology rules
    it_channels = {}
    for eid, (st, body, name) in sorted(entities.items()):
        if st == 2:
            ttype = struct.unpack_from("<H", body, 4)[0]
            it_channels[eid] = body[8]
            a.note("IT id=%d wTerminalType=0x%04X bCSourceID=%d bNrChannels=%d"
                   % (eid, ttype, body[7], body[8]))
            if body[0] != 17:
                a.err("Input Terminal id=%d bLength=%d (17 expected)" % (eid, body[0]))
            if body[7] not in entities:
                a.err("Input Terminal id=%d bCSourceID=%d is not an entity"
                      % (eid, body[7]))
        elif st == 3:
            ttype = struct.unpack_from("<H", body, 4)[0]
            a.note("OT id=%d wTerminalType=0x%04X bSourceID=%d bCSourceID=%d"
                   % (eid, ttype, body[7], body[8]))
            if body[0] != 12:
                a.err("Output Terminal id=%d bLength=%d (12 expected)" % (eid, body[0]))
            if body[7] not in entities:
                a.err("Output Terminal id=%d bSourceID=%d is not an entity"
                      % (eid, body[7]))
            if body[8] not in entities:
                a.err("Output Terminal id=%d bCSourceID=%d is not an entity"
                      % (eid, body[8]))
        elif st == 6:
            if (body[0] - 6) % 4 != 0:
                a.err("Feature Unit id=%d bLength=%d is not 6+4n" % (eid, body[0]))
            nctrl = (body[0] - 6) // 4
            ch = nctrl - 1
            src = body[4]
            src_ch = it_channels.get(src)
            a.note("FU id=%d bSourceID=%d bLength=%d -> %d bmaControls -> %d channel(s)"
                   % (eid, src, body[0], nctrl, ch))
            if src not in entities:
                a.err("Feature Unit id=%d bSourceID=%d is not an entity" % (eid, src))
            if src_ch is not None and ch != src_ch:
                a.err("Feature Unit id=%d declares %d channel(s) but its source "
                      "carries %d -> UAC2 bLength must be 6+(n+1)*4 = %d"
                      % (eid, ch, src_ch, 6 + (src_ch + 1) * 4))
            if nctrl >= 1 and body[5] == 0:
                a.warn("Feature Unit id=%d master bmaControls=0 (no mute/volume "
                       "advertised)" % eid)
        elif st == 0x0A:
            a.note("CLOCK id=%d bmAttributes=0x%02X bmControls=0x%02X"
                   % (eid, body[4], body[5]))
            if body[0] != 8:
                a.err("Clock Source id=%d bLength=%d (8 expected)" % (eid, body[0]))
            if (body[5] & 0x03) != 0x03:
                a.err("Clock Source id=%d bmControls=0x%02X: Clock Frequency "
                      "read+write (0x03) must be advertised so the host can set "
                      "the sample rate" % (eid, body[5]))
            if body[4] & 0x01:
                a.warn("Clock Source id=%d bmAttributes bit0 (External Clock) is "
                       "set; an internal clock is normally expected here" % eid)
    if not any(st == 0x0A for st, _, _ in entities.values()):
        a.err("UAC2 requires at least one Clock Source entity")

    # ---- AS rules
    for asf in as_list:
        body = asf["desc"]
        link = body[3]
        nch = body[10]
        fmt = struct.unpack_from("<I", body, 6)[0]
        a.note("AS if %d alt %d bTerminalLink=%d bNrChannels=%d bmFormats=0x%08X"
               % (asf["ifnum"], asf["alt"], link, nch, fmt))
        if link not in entities:
            a.err("AS if %d bTerminalLink=%d is not an entity in the AudioControl "
                  "interface" % (asf["ifnum"], link))
        else:
            st, tgt, nm = entities[link]
            ttype = struct.unpack_from("<H", tgt, 4)[0]
            if st != 2:
                a.err("AS if %d bTerminalLink=%d references %s; a playback "
                      "streaming interface must link to the USB-Streaming INPUT "
                      "terminal (subtype 2)" % (asf["ifnum"], link, nm))
            elif ttype != 0x0101:
                a.err("AS if %d bTerminalLink terminal type=0x%04X; USB Streaming "
                      "(0x0101) expected on the playback path" % (asf["ifnum"], ttype))
            else:
                a.note("  -> AS if %d correctly links to Input Terminal %d "
                       "(USB Streaming)" % (asf["ifnum"], link))
        if fmt == 0:
            a.err("AS if %d bmFormats=0 (no format advertised)" % asf["ifnum"])
        if asf["alt"] == 0:
            a.err("AS General descriptor found in alt setting 0; it belongs to the "
                  "operational alt setting (alt 1)")
        if asf["iface"]["nep"] == 0:
            a.err("AS interface %d alt %d has bNumEndpoints=0 but carries a "
                  "class-specific AS General descriptor" % (asf["ifnum"], asf["alt"]))
        if link in it_channels and nch != it_channels[link]:
            a.err("AS if %d bNrChannels=%d != Input Terminal %d channels=%d"
                  % (asf["ifnum"], nch, link, it_channels[link]))

    # ---- endpoint / feedback rules
    all_eps = [(k, e) for k, v in interfaces.items() for e in v["endpoints"]]
    data_iso = [(k, e) for k, e in all_eps
                if (e["attr"] & 3) == 1 and (e["attr"] & 0x0C) != 0x0C]
    fb_iso = [(k, e) for k, e in all_eps
              if (e["attr"] & 3) == 1 and ((e["attr"] >> 4) & 3) == 1]
    for k, e in fb_iso:
        a.note("feedback EP 0x%02X mps=%d bInterval=%d" % (e["addr"], e["mps"], e["ival"]))
        if e["mps"] != 4:
            a.err("feedback EP 0x%02X wMaxPacketSize=%d (must be 4: 16.16 PCM "
                  "feedback)" % (e["addr"], e["mps"]))
        if e["ival"] != 1:
            a.warn("feedback EP 0x%02X bInterval=%d (1 means every microframe on HS)"
                   % (e["addr"], e["ival"]))
    for k, e in data_iso:
        if (e["attr"] & 0x0C) == 0x04 and not fb_iso:
            a.err("asynchronous isochronous data EP 0x%02X without a feedback "
                  "endpoint" % e["addr"])
    # endpoint address reuse across interfaces of the same function
    seen = {}
    for k, e in all_eps:
        if e["addr"] in seen and seen[e["addr"]] != k:
            a.err("endpoint 0x%02X declared in both IF %s and IF %s"
                  % (e["addr"], seen[e["addr"]], k))
        seen[e["addr"]] = k
    # a single endpoint index used in both directions with different sizes is legal
    # in the descriptor, but the CH32 USBHS port has ONE max-len register per index.
    return interfaces, entities, as_list, iads, all_eps


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else r"obj\LittleTail.elf"
    want = sys.argv[2:] or ["device_desc", "conf_desc", "g_audio_fs_table"]
    elf = read_elf(path)
    secs, syms = elf["secs"], elf["syms"]
    byname = {s["name"]: s for s in syms}
    # static locals appear as "name.NNNN"; accept a unique prefix match too
    def lookup(n):
        if n in byname:
            return byname[n]
        cand = [s for k, s in byname.items() if k.split(".")[0] == n]
        return cand[0] if len(cand) == 1 else None

    a = Audit()

    for name in want:
        sym = lookup(name)
        if sym is None:
            print("!! symbol %s not found" % name)
            continue
        b = bytes_of(elf, secs, sym)
        print("=== %s : %d bytes ===" % (name, len(b)))
        print(hexs(b))
        print()
        if name == "device_desc":
            print("  bcdUSB=0x%04X class=0x%02X sub=0x%02X proto=0x%02X "
                  "bMaxPacketSize0=%d VID=0x%04X PID=0x%04X bcdDevice=0x%04X "
                  "iMfr=%d iProd=%d iSer=%d nConfigs=%d"
                  % (struct.unpack_from("<H", b, 2)[0], b[4], b[5], b[6], b[7],
                     struct.unpack_from("<H", b, 8)[0],
                     struct.unpack_from("<H", b, 10)[0],
                     struct.unpack_from("<H", b, 12)[0], b[14], b[15], b[16], b[17]))
            if b[7] != 64:
                a.err("bMaxPacketSize0=%d (HS requires 64)" % b[7])
        elif name == "conf_desc":
            check_config(b, a)
        elif name == "g_audio_fs_table":
            num = struct.unpack_from("<H", b, 0)[0]
            print("  UAC2 RANGE: wNumSubRanges=%d" % num)
            off = 2
            for i in range(num):
                mn, mx, rs = struct.unpack_from("<III", b, off)
                off += 12
                print("    dMin=%d dMax=%d dRes=%d" % (mn, mx, rs))
                if mn > mx:
                    a.err("RANGE subrange %d: dMin > dMax" % i)
            if off != len(b):
                a.err("RANGE table is %d bytes but %d subranges need %d bytes"
                      % (len(b), num, off))
        print()

    print("=========== RESULT ===========")
    for m in a.info:
        print("  .    " + m)
    for m in a.warns:
        print("  WARN " + m)
    for m in a.errors:
        print("  ERR  " + m)
    print("errors=%d warnings=%d" % (len(a.errors), len(a.warns)))
    return 1 if a.errors else 0


if __name__ == "__main__":
    sys.exit(main())
