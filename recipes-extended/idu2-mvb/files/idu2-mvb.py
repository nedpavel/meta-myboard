#!/usr/bin/env python3
"""IDU2 am MVB: Lebenszeichen ab dem Start, DDS-Abfrage an DIA1 auf
Anfrage.

Der Dienst haelt das Display als IDU2 (Geraet 240) dauernd am Bus. Weil
/dev/mvblli0 exklusiv ist, laeuft alles ueber ihn - auch die DDS-Abfrage
der Bedienoberflaeche. Die kommt so ohne Initialisierungszeit aus: der
Controller laeuft schon, das Lebenszeichen auch.

Lebenszeichen: Telegramm IDU2-491 laut Busbeschreibung (MVB (trak).txt),
Port 491, 4 Byte, Zyklus 256 ms; IDU2-XMVBLifeSig als CARD16 big-endian ab
Byte 0, gelesen von FLG1N, FLG2N und IDU1, im Sekundentakt hochgezaehlt.
Byte 2 traegt Luefterbefehle an FLG1N und bleibt 0. Ohne laufendes
Lebenszeichen gilt IDU2 fuer FLG und DIA1 als nicht da.

Einrichtung wie im erfolgreichen Lauf stufe8/ef1 (2026-10-12): Adresse
240, Einleitungsbetrieb auf Leitung A, MD-Bit im DSW, Anmeldung von
Messages ueber EF1 (MD_CONF mit negativer Prioritaet).

DDS-Abfrage: dieselben Frames wie ddsabfrage.py (Mitschnitt des Displays).
IDU2 schickt die Anfrage an DIA1 (Geraet 46), beantwortet CC/AK und die
Reply-Message von DIA1, nimmt die Daten (Teil 2) entgegen und quittiert
sie mit der Reply des Displays. Geantwortet wird nur auf Frames von
DIA1. Ruft DIA1 von sich aus an, beantwortet der Dienst das ebenfalls und
haelt das Ergebnis bereit.

Steuerung ueber den Unix-Socket /run/idu2-mvb.sock, je Anfrage und
Antwort eine JSON-Zeile:

    {"befehl": "dds"}      DDS-Abfrage; Antwort mit allen Meldungssaetzen
    {"befehl": "letzte"}   letztes Ergebnis, ohne neue Abfrage
    {"befehl": "status"}   Lebenszeichen, Frames, laufende Abfrage

/dev/mvblli0 ist exklusiv. Fuer ddsabfrage.py und mvbdiff.py:

    systemctl stop idu2-mvb     ...     systemctl start idu2-mvb

Optionen (auch ueber OPTIONEN in /etc/default/idu2-mvb):
    --zwei-leitungen   Zweileitungsbetrieb statt Leitung A allein
    --ohne-dsw-md      MD-Bit im DSW nicht setzen
    --ef0              Messages ueber EF0 statt EF1 anmelden
    --takt S           Lebenszeichen alle S Sekunden erhoehen (Vorgabe 1)
    --alle-quellen     auch 492 (8 Byte) und 498 (16 Byte) senden, Nullen
    --socket PFAD      Steuersocket (Vorgabe /run/idu2-mvb.sock)
"""

import errno
import fcntl
import json
import os
import select
import signal
import socket
import struct
import sys
import time

DEV = "/dev/mvblli0"
SOCK = "/run/idu2-mvb.sock"
OWN_ADDR = 240
DIA1_ADDR = 0x2E
ABFRAGE_TIMEOUT = 15.0

IOC = {
    "WRITE_DEV_ADDR": 0x40024C02,
    "WRITE_DSW":      0x40044C04,
    "START":          0x00004C05,
    "STOP":           0x00004C06,
    "PD_CONF":        0x40024C0C,
    "MD_CONF":        0x40024C0E,
    "WRITE_CONTROL":  0x40064C14,
    "HWINIT":         0x40104C15,
}
PD, MD_LOW = 1, 3
PD_SRC = 2
LS_PORT, LS_SIZE = 491, 4
EXTRA_SRC = ((492, 8), (498, 16))
DSW_MD = 0x1000
CTRL_CLEAR, CTRL_LINE_A = 0x03, 0x08

# ---------------------------------------------------------- Message-Frames
# Aufbau wie in ddsabfrage.py: 0..3 Linkkopf (setzt der Treiber), 4 SZ,
# 5..8 Netzkopf, 9 MTC (Bit 7 Caller-Seite, Bits 6..4 Typ 0 CR / 4 CC /
# 1 DT / 6 AK, bei DT Bit 3 letztes Paket und Bits 2..0 Folgenummer);
# CR: 10..11 Ref, 12..15 Laenge, 16 Credit, 18.. Daten; CC: 10..11 Ref,
# 12 Credit; DT: 10.. Daten.

KINDS = {0: "CR", 4: "CC", 1: "DT", 6: "AK"}

# Aus dem Mitschnitt des Displays, ab Offset 4 (SZ). Ref wird ersetzt.
CALL_CR = bytes.fromhex(
    "1b 00e00000 80 0043 0000001c 70 00 0100000001 0501010101 00010415"
    .replace(" ", ""))
CALL_DT = bytes.fromhex(
    "13 00e00000 98 6000475200000100420101200000 0101010100010415"
    .replace(" ", ""))
REPLY_DATA = bytes.fromhex("0100000000010501fb01")
REPLY_PAD = bytes.fromhex("00010415")
CC_CREDIT = 0x10


def be16(b, o):
    return (b[o] << 8) | b[o + 1]


def frame(tail):
    f = bytes(4) + tail
    return f + bytes(32 - len(f))


def swap_net(net):
    return bytes((net[2], net[3], net[0], net[1]))


def decode(f):
    d = {"src": ((f[2] & 0x0F) << 8) | f[3], "sz": f[4],
         "net": bytes(f[5:9]), "mtc": f[9]}
    d["caller"] = bool(d["mtc"] & 0x80)
    d["kind"] = KINDS.get((d["mtc"] >> 4) & 7, "?")
    end = 5 + d["sz"]
    if d["kind"] == "CR":
        d["ref"] = be16(f, 10)
        d["len"] = struct.unpack(">I", bytes(f[12:16]))[0]
        d["data"] = bytes(f[18:end])
    elif d["kind"] == "CC":
        d["ref"] = be16(f, 10)
    elif d["kind"] == "DT":
        d["seq"] = d["mtc"] & 7
        d["data"] = bytes(f[10:end])
    return d


# ---------------------------------------------------------- DDS-Meldungen
# Wie ddsabfrage.py: Kopf 14 Byte, Saetze ab 00 80 00 im 33-Byte-Raster
# (der letzte ohne Nr), danach je Satz ein Klartext (CP850, nullterminiert).

DDS_MARK = b"\x00\x80\x00"
DDS_REC = 33
NO_TIME = "--.--.---- --:--:--.---"
PRIO = {0x10: "A", 0x20: "B", 0x30: "C"}


def dds_time(sec):
    if sec == 0xFFFFFFFF:
        return NO_TIME
    return time.strftime("%d.%m.%Y %H:%M:%S", time.gmtime(sec)) + ".000"


def dds_meldungen(msg):
    """Liste der Meldungen als dicts, oder None ohne DDS-Saetze."""
    i0 = msg.find(DDS_MARK)
    if i0 < 0:
        return None
    n = 0
    while i0 + DDS_REC * n + 32 <= len(msg) and \
            msg[i0 + DDS_REC * n:i0 + DDS_REC * n + 3] == DDS_MARK:
        n += 1
    if not n:
        return None
    texts = [t.decode("cp850", errors="replace")
             for t in msg[i0 + DDS_REC * n - 1:].split(b"\x00")]
    out = []
    for k in range(n):
        r = msg[i0 + DDS_REC * k:i0 + DDS_REC * k + DDS_REC]
        u16 = lambda o: struct.unpack_from("<H", r, o)[0]
        u32 = lambda o: struct.unpack_from("<I", r, o)[0]
        geht = u32(13)
        out.append({
            "nr": r[32] if k < n - 1 else k + 1,
            "kommt": dds_time(u32(9)),
            "geht": dds_time(geht),
            "status": "aktiv" if geht == 0xFFFFFFFF else "gegangen",
            "stoer": u16(27) & 0x7FFF,
            "knoten": "%02X" % r[29],
            "knoten2": "%02X" % r[30],
            "prio": PRIO.get(r[31], "%02X" % r[31]),
            "zaehler": u16(25),
            "uic": "%04X" % u16(19),
            "ref7": u16(21),
            "ref8": u16(23),
            "ereignisnr": struct.unpack_from(">H", r, 3)[0],
            "meldnr": struct.unpack_from(">H", r, 7)[0],
            "text": texts[k] if k < len(texts) else "",
            "roh": bytes(r[:DDS_REC if k < n - 1 else 32]).hex(" "),
        })
    return out


# ------------------------------------------------------------------ Dienst


def log(msg):
    # stdout landet im Journal: journalctl -u idu2-mvb
    print(msg, flush=True)


def parse_args(argv):
    o = {"single": True, "dsw_md": True, "ef1": True, "takt": 1.0,
         "alle": False, "socket": SOCK}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--zwei-leitungen":
            o["single"] = False
        elif a == "--ohne-dsw-md":
            o["dsw_md"] = False
        elif a == "--ef0":
            o["ef1"] = False
        elif a == "--alle-quellen":
            o["alle"] = True
        elif a == "--takt":
            i += 1
            o["takt"] = float(argv[i])
        elif a == "--socket":
            i += 1
            o["socket"] = argv[i]
        else:
            log("Unbekannte Option: %s" % a)
            sys.exit(2)
        i += 1
    return o


class Mvb:
    """/dev/mvblli0: Einrichtung, Prozess- und Message-Daten."""

    def __init__(self, fd):
        self.fd = fd

    def io(self, name, arg=0):
        req = IOC[name]
        req = req - (1 << 32) if req >= (1 << 31) else req
        if isinstance(arg, bytearray):
            return fcntl.ioctl(self.fd, req, arg, True)
        return fcntl.ioctl(self.fd, req, arg)

    def pd_write(self, port, data):
        # Der Treiber liest die vollen 38 Byte, als Laenge sieht er die
        # Portgroesse
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, PD, port)
        buf[4:4 + len(data)] = data
        os.writev(self.fd, [memoryview(buf)[:len(data)]])

    def md_write(self, dest, tail):
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, MD_LOW, dest)
        buf[4:36] = frame(tail)
        os.writev(self.fd, [memoryview(buf)[:32]])

    def md_read(self):
        """Naechstes empfangenes Frame oder None."""
        buf = bytearray(38)
        struct.pack_into("<HH", buf, 0, MD_LOW, DIA1_ADDR)
        try:
            os.readv(self.fd, [memoryview(buf)[:32]])
        except OSError as e:
            if e.errno in (errno.ENOBUFS, errno.EAGAIN, errno.ENETDOWN):
                return None
            raise
        return bytes(buf[4:36])


class Dia1:
    """Message-Transport mit DIA1: beantwortet dessen Frames, fuehrt die
    DDS-Abfrage und liefert das Ergebnis an den wartenden Client."""

    def __init__(self, mvb):
        self.mvb = mvb
        self.next_ref = 0x0043
        self.conns = {}
        self.pending = None     # laufende Abfrage
        self.letzte = None      # letztes Ergebnis (dict fuer JSON)
        self.rx = 0
        self.fremd = 0

    def alloc_ref(self):
        ref = self.next_ref
        self.next_ref = (self.next_ref + 1) & 0xFFFF or 1
        return ref

    def tx(self, tail):
        self.mvb.md_write(DIA1_ADDR, tail)

    def start(self, reply_to):
        ref = self.alloc_ref()
        self.pending = {"ref": ref, "t0": time.time(), "cc": False,
                        "antwort": None, "reply_to": reply_to}
        self.tx(CALL_CR[:6] + struct.pack(">H", ref) + CALL_CR[8:])
        log("DDS-Abfrage an DIA1 gesendet, Ref %04X" % ref)

    def check_timeout(self):
        p = self.pending
        if p and time.time() - p["t0"] > ABFRAGE_TIMEOUT:
            self.pending = None
            grund = ("DIA1 hat die Anfrage nicht angenommen (kein CC)"
                     if not p["cc"] else "keine DDS-Daten von DIA1")
            log("DDS-Abfrage Ref %04X: %s" % (p["ref"], grund))
            return p["reply_to"], {"ok": False, "fehler": grund}
        return None, None

    def handle(self, f):
        """Ein empfangenes Frame. Liefert (Client, Antwort) oder
        (None, None)."""
        self.rx += 1
        d = decode(f)
        if d["src"] != DIA1_ADDR:
            self.fremd += 1
            return None, None
        back = swap_net(d["net"])
        side = 0x00 if d["caller"] else 0x80     # wir: die Gegenseite
        key = d["net"]
        if d["kind"] == "CR":
            self.tx(bytes((0x08,)) + back + bytes((side | 0x41,))
                    + struct.pack(">H", d["ref"]) + bytes((CC_CREDIT,)))
            self.conns[key] = {"ref": d["ref"], "len": d["len"],
                               "data": bytearray(d["data"]),
                               "call": d["caller"], "net": d["net"]}
            return self.check_done(key)
        if d["kind"] == "DT":
            self.tx(bytes((0x05,)) + back
                    + bytes((side | 0x60 | ((d["seq"] + 1) & 7),)))
            c = self.conns.get(key)
            if c is None:
                return None, None
            c["data"] += d["data"]
            return self.check_done(key)
        if d["kind"] == "CC":
            p = self.pending
            if p and d["ref"] == p["ref"] and not p["cc"]:
                p["cc"] = True
                self.tx(CALL_DT)
        return None, None

    def check_done(self, key):
        c = self.conns[key]
        if len(c["data"]) < c["len"]:
            return None, None
        msg = bytes(c["data"][:c["len"]])
        del self.conns[key]
        p = self.pending
        if not c["call"]:
            # Reply von DIA1 auf unsere Anfrage
            if p:
                p["antwort"] = msg.hex()
            return None, None
        # Daten von DIA1 (Teil 2): Reply des Displays, dann auswerten
        ref = self.alloc_ref()
        self.tx(bytes((0x17,)) + swap_net(c["net"]) + bytes((0x00,))
                + struct.pack(">HI", ref, len(REPLY_DATA))
                + bytes((0x70, 0x00)) + REPLY_DATA + REPLY_PAD)
        meld = dds_meldungen(msg) or []
        erg = {"ok": True, "anzahl": len(meld), "bytes": len(msg),
               "zeit": time.strftime("%d.%m.%Y %H:%M:%S"),
               "meldungen": meld}
        if p:
            erg["dauer_s"] = round(time.time() - p["t0"], 2)
            if p["antwort"]:
                erg["antwort"] = p["antwort"]
        self.letzte = erg
        self.pending = None
        log("DDS-Daten von DIA1: %d Byte, %d Meldungen" % (len(msg),
                                                          len(meld)))
        return (p["reply_to"] if p else None), erg


def main():
    o = parse_args(sys.argv)
    stop = {"jetzt": False}

    def on_signal(signum, frame_):
        stop["jetzt"] = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    # /dev/mvblli0 kann kurz nach dem Booten noch fehlen
    t0 = time.time()
    while True:
        try:
            fd = os.open(DEV, os.O_RDWR)
            break
        except OSError as e:
            if time.time() - t0 > 60 or e.errno == errno.EBUSY:
                log("%s nicht zu oeffnen: %s%s" % (
                    DEV, os.strerror(e.errno),
                    " - eine andere Anwendung haelt es"
                    if e.errno == errno.EBUSY else ""))
                return 1
            time.sleep(1)

    mvb = Mvb(fd)
    wert = 0
    try:
        ports = [(LS_PORT, LS_SIZE)] + (list(EXTRA_SRC) if o["alle"] else [])
        cfg = struct.pack("<H", len(ports))
        for p, s in ports:
            cfg += struct.pack("<HHH", p, s, PD_SRC)
        mvb.io("WRITE_DEV_ADDR", OWN_ADDR)
        mvb.io("HWINIT", bytearray(struct.pack("<QBBHHB x",
                                               0, 1, 1, 0xFFF, 0xFFF, 0)))
        mvb.io("MD_CONF", bytearray(struct.pack("<H",
                                                0xFFFF if o["ef1"] else 0)))
        mvb.io("PD_CONF", bytearray(cfg))
        # Quellen werden erst mit dem ersten write() aktiv
        mvb.pd_write(LS_PORT, struct.pack(">HH", wert, 0))
        for p, s in ports[1:]:
            mvb.pd_write(p, bytes(s))
        cmd = CTRL_CLEAR | (CTRL_LINE_A if o["single"] else 0)
        mvb.io("WRITE_CONTROL", bytearray(struct.pack("<HHBx", OWN_ADDR, 43,
                                                      cmd)))
        if o["dsw_md"]:
            mvb.io("WRITE_DSW", (DSW_MD << 16) | DSW_MD)
        mvb.io("START")
    except OSError as e:
        log("Einrichtung fehlgeschlagen: %s" % os.strerror(e.errno))
        os.close(fd)
        return 1

    try:
        os.unlink(o["socket"])
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(o["socket"])
    os.chmod(o["socket"], 0o666)
    srv.listen(4)
    srv.setblocking(False)
    clients = {}            # socket -> Eingangspuffer

    log("IDU2 (Geraet %d) am MVB: Lebenszeichen auf Port %d alle %g s, %s, "
        "%s, Messages ueber %s%s; Steuerung %s"
        % (OWN_ADDR, LS_PORT, o["takt"],
           "Leitung A allein" if o["single"] else "zwei Leitungen",
           "MD-Bit im DSW" if o["dsw_md"] else "DSW ohne MD-Bit",
           "EF1" if o["ef1"] else "EF0",
           ", dazu 492/498" if o["alle"] else "", o["socket"]))

    dia = Dia1(mvb)

    def send(cl, obj):
        if cl is None or cl not in clients:
            return
        try:
            cl.sendall(json.dumps(obj, ensure_ascii=False).encode("utf-8")
                       + b"\n")
        except OSError:
            clients.pop(cl, None)
            cl.close()

    def request(cl, line):
        try:
            req = json.loads(line)
            befehl = req.get("befehl")
        except (ValueError, AttributeError):
            send(cl, {"ok": False, "fehler": "keine gueltige JSON-Anfrage"})
            return
        if befehl == "dds":
            if dia.pending:
                send(cl, {"ok": False, "fehler": "Abfrage laeuft bereits"})
                return
            try:
                dia.start(cl)
            except OSError as e:
                send(cl, {"ok": False, "fehler": "Senden fehlgeschlagen: %s"
                          % os.strerror(e.errno)})
        elif befehl == "letzte":
            send(cl, dia.letzte or {"ok": False,
                                    "fehler": "noch keine DDS-Daten"})
        elif befehl == "status":
            send(cl, {"ok": True, "lebenszeichen": wert,
                      "abfrage_laeuft": bool(dia.pending),
                      "frames_empfangen": dia.rx,
                      "frames_andere_geraete": dia.fremd})
        else:
            send(cl, {"ok": False, "fehler": "unbekannter Befehl"})

    fehler = 0
    naechster = time.time() + o["takt"]
    letzte_meldung = time.time()
    while not stop["jetzt"]:
        wait = 0.01 if dia.pending or dia.conns else 0.1
        try:
            r, _, _ = select.select([srv] + list(clients), [], [], wait)
        except InterruptedError:
            continue
        for s in r:
            if s is srv:
                try:
                    cl, _ = srv.accept()
                    cl.setblocking(False)
                    clients[cl] = b""
                except OSError:
                    pass
                continue
            try:
                data = s.recv(4096)
            except OSError:
                data = b""
            if not data:
                clients.pop(s, None)
                s.close()
                continue
            clients[s] += data
            while s in clients and b"\n" in clients[s]:
                line, clients[s] = clients[s].split(b"\n", 1)
                if line.strip():
                    request(s, line.decode("utf-8", errors="replace"))

        # Empfangene Message-Frames leer lesen - auch ohne Abfrage, sonst
        # laeuft der Ring des Treibers ueber
        while True:
            try:
                f = mvb.md_read()
            except OSError as e:
                log("Message-Empfang: %s" % os.strerror(e.errno))
                break
            if f is None:
                break
            try:
                cl, erg = dia.handle(f)
            except OSError as e:
                log("Message-Senden: %s" % os.strerror(e.errno))
                continue
            if erg is not None:
                send(cl, erg)
        cl, erg = dia.check_timeout()
        if erg is not None:
            send(cl, erg)

        if time.time() >= naechster:
            naechster += o["takt"]
            wert = (wert + 1) & 0xFFFF
            try:
                mvb.pd_write(LS_PORT, struct.pack(">HH", wert, 0))
                if fehler:
                    log("Lebenszeichen wieder geschrieben (Wert %04X)" % wert)
                    fehler = 0
            except OSError as e:
                fehler += 1
                if fehler == 1:
                    log("Lebenszeichen nicht geschrieben: %s"
                        % os.strerror(e.errno))
            if time.time() - letzte_meldung >= 600:
                log("Lebenszeichen laeuft, Wert %04X, Frames %d (andere "
                    "Geraete %d)" % (wert, dia.rx, dia.fremd))
                letzte_meldung = time.time()

    # STOP und close() nehmen das Geraet vom Bus und setzen den
    # Controller zurueck
    for cl in list(clients):
        cl.close()
    srv.close()
    try:
        os.unlink(o["socket"])
    except OSError:
        pass
    try:
        mvb.io("STOP")
    except OSError:
        pass
    if o["dsw_md"]:
        try:
            mvb.io("WRITE_DSW", DSW_MD << 16)
        except OSError:
            pass
    os.close(fd)
    log("beendet, letztes Lebenszeichen %04X" % wert)
    return 0


sys.exit(main())
