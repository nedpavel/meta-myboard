#!/usr/bin/env python3
"""myboard-webgui-api - Hintergrund-Dienst der Web-Oberflaeche.

Die Web-Oberflaeche (/gui/, HTML/JavaScript in Firefox) kann weder CAN noch
den Socket von idu2-mvb noch USB-Sticks selbst ansprechen; PHP-FPM darf es
auch nicht (laeuft als nobody, ohne AF_CAN). Dieser Dienst ist die Bruecke.
Apache reicht /api/ an ihn weiter (127.0.0.1:8081).

    GET  /api/can/strom   Server-Sent Events: zuerst die ganze Tabelle, dann
                          hoechstens 4x/s die geaenderten Zeilen
    GET  /api/can/stand   die ganze Tabelle als JSON
    POST /api/dds         {"seite": N, "neu": bool} -> idu2-mvb, Antwort 1:1
    GET  /api/softkeys    Softkeys der Rolle mit Zustand und Rueckmeldungen
                          (idu2-mvb "softkeys"), Antwort 1:1
    POST /api/softkey     {"signal": "IDU2-BTestSound"} -> idu2-mvb "softkey"
                          (Puls); erlaubt sind nur Signale der Softkey-Liste
    GET  /api/usb         {"stick": bool}
    POST /api/update      Web-App vom USB-Stick einspielen
    GET  /api/info        Angaben fuer die Info-Seite

CAN wie myboard-gui (CanReader, CanMatrix, canopenDescribe): can0 roh, eine
Zeile je Matrix-Signal, sonst je CAN-ID eine graue CANopen-/Hex-Zeile; die
Texte sind dieselben. Anders als die Qt-App versucht der Dienst can0 alle
5 s erneut, falls es beim Start noch fehlt.

Update: im Wurzelverzeichnis des Sticks ein *.tar.gz, das www/index.html
enthaelt. Es wird neben das laufende www entpackt und dann per Umbenennen
getauscht (www -> www.alt, www.neu -> www).

Optionen:
    --port N          Port (Vorgabe 8081)
    --adresse A       Adresse (Vorgabe 127.0.0.1; Apache davor)
    --can IF          CAN-Schnittstelle (Vorgabe can0)
    --can-quelle F    statt CAN ein candump-Log abspielen (Test ohne CAN)
    --matrix F        Signaltabelle (Vorgabe /usr/share/myboard-webgui/can_matrix.tsv)
    --www D           Web-App (Vorgabe /usr/share/myboard-webgui/www);
                      wird unter /gui/ auch direkt ausgeliefert (Test ohne Apache)
    --dds-socket S    Socket von idu2-mvb (Vorgabe /run/idu2-mvb.sock)
"""

import errno
import glob
import json
import mimetypes
import os
import shutil
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

VERSION = "1.0"
CAN_ERR_FLAG, CAN_EFF_FLAG = 0x20000000, 0x80000000
CAN_EFF_MASK, CAN_SFF_MASK = 0x1FFFFFFF, 0x000007FF
SSE_TAKT = 0.25         # hoechstens 4 Pakete/s je Browser
DDS_TIMEOUT = 60.0      # wie DdsClient (TIMEOUT_MS)
SOFTKEY_TIMEOUT = 3.0   # wie iduAnfrage in Qt
UPDATE_MNT = "/mnt/usb-update"


def log(msg):
    print(msg, flush=True)


def parse_args(argv):
    o = {"port": 8081, "adresse": "127.0.0.1", "can": "can0", "quelle": None,
         "matrix": "/usr/share/myboard-webgui/can_matrix.tsv",
         "www": "/usr/share/myboard-webgui/www", "dds": "/run/idu2-mvb.sock"}
    namen = {"--port": "port", "--adresse": "adresse", "--can": "can",
             "--can-quelle": "quelle", "--matrix": "matrix", "--www": "www",
             "--dds-socket": "dds"}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a not in namen or i + 1 >= len(argv):
            log("Unbekannte Option: %s" % a)
            sys.exit(2)
        o[namen[a]] = int(argv[i + 1]) if a == "--port" else argv[i + 1]
        i += 2
    return o


# ------------------------------------------------------------ CAN-Matrix
# Wie canmatrix.cpp: TSV mit can_id bit_offset width signed factor offset
# unit name [comment]; Bits little-endian, Bit 0 = LSB von Byte 0.

class CanMatrix:
    def __init__(self):
        self.by_id = {}
        self.count = 0

    def laden(self, pfad):
        self.by_id, self.count = {}, 0
        try:
            f = open(pfad, "rb")
        except OSError:
            return False
        with f:
            for raw in f:
                if not raw or raw.startswith(b"#"):
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                c = line.split("\t")
                if len(c) < 8 or not c[0].isdigit():
                    continue
                try:
                    s = {"off": int(c[1]), "width": int(c[2]),
                         "signed": int(c[3]) != 0, "factor": float(c[4]),
                         "offset": float(c[5]), "unit": c[6], "name": c[7],
                         "comment": c[8] if len(c) >= 9 else ""}
                except ValueError:
                    continue
                if s["width"] <= 0 or s["width"] > 32:
                    continue
                self.by_id.setdefault(int(c[0]), []).append(s)
                self.count += 1
        return self.count > 0

    def decode(self, can_id, data):
        """[(name, label, unit, wert_text)] wie MainWindow::onCanFrame."""
        out = []
        n = len(data)
        for s in self.by_id.get(can_id, ()):
            raw = 0
            for i in range(s["width"]):
                bit = s["off"] + i
                byte = bit >> 3
                if 0 <= byte < n:
                    raw |= ((data[byte] >> (bit & 7)) & 1) << i
            num = float(raw)
            if s["signed"] and raw & (1 << (s["width"] - 1)):
                num = float(raw - (1 << s["width"]))
            value = num * s["factor"] + s["offset"]
            out.append((s["name"], s["comment"] or s["name"], s["unit"],
                        wert_text(value, raw, s["width"] == 1)))
        return out


def qround64(v):
    # qRound64 von Qt: ab ,5 vom Nullpunkt weg
    return int(v + 0.5) if v >= 0.0 else int(v - 0.5)


def wert_text(value, raw, is_bool):
    if is_bool:
        return "1" if raw else "0"
    if abs(value - qround64(value)) < 1e-9:
        return str(qround64(value))
    return "%.2f" % value


# ------------------------------------------------------------ CANopen
# Wie canopen.cpp - fuer IDs, die die Matrix nicht kennt.

def can_hex(d):
    return " ".join("%02X" % x for x in d)


def _b(d, i):
    return d[i] if i < len(d) else 0


def _hx(v, width=0):
    return "0x" + ("%X" % v).rjust(width, "0")


OBJ_NAMEN = {0x1000: " (Device Type)", 0x1001: " (Error Register)",
             0x1008: " (Device Name)", 0x1009: " (HW-Version)",
             0x100A: " (SW-Version)", 0x1017: " (Heartbeat-Time)",
             0x1018: " (Identity)"}


def _idx_sub(d):
    index = _b(d, 1) | (_b(d, 2) << 8)
    return _hx(index, 4) + ":" + ("%X" % _b(d, 3)).rjust(2, "0") \
        + OBJ_NAMEN.get(index, "")


def _expedited(d, cmd):
    if not (cmd >> 1) & 1:
        return "(segmentiert)"
    size = 4 - ((cmd >> 2) & 3)
    raw = bytes(_b(d, 4 + i) for i in range(size))
    v = int.from_bytes(raw, "little")
    return _hx(v) + " (%d) [" % v + can_hex(raw) + "]"


PDO_NAMEN = {0x3: "TPDO1", 0x4: "RPDO1", 0x5: "TPDO2", 0x6: "RPDO2",
             0x7: "TPDO3", 0x8: "RPDO3", 0x9: "TPDO4", 0xA: "RPDO4"}


def canopen_describe(cid, d):
    """(ok, label, value) wie canopenDescribe()."""
    node, fc = cid & 0x7F, (cid >> 7) & 0xF
    if cid == 0x000:
        cmd, tgt = _b(d, 0), _b(d, 1)
        c = {0x01: "Start", 0x02: "Stop", 0x80: "Pre-Operational",
             0x81: "Reset Node", 0x82: "Reset Communication"}.get(
                 cmd, "cmd " + _hx(cmd, 2))
        return True, "NMT", c + " -> " + ("alle Nodes" if tgt == 0
                                          else "Node %d" % tgt)
    if cid == 0x080:
        return True, "SYNC", ""
    if cid == 0x100:
        return True, "TIME", ""
    if fc == 0x1:
        return True, "EMCY Node %d" % node, (
            "Fehlercode " + _hx(_b(d, 0) | (_b(d, 1) << 8), 4)
            + ", Error-Reg " + _hx(_b(d, 2), 2))
    if fc in PDO_NAMEN:
        return True, "%s Node %d" % (PDO_NAMEN[fc], node), can_hex(d)
    if fc == 0xB:
        cmd = _b(d, 0)
        scs = cmd >> 5
        if scs == 0x4:
            v = "Abort " + _idx_sub(d) + " (" + _hx(
                _b(d, 4) | (_b(d, 5) << 8) | (_b(d, 6) << 16) | (_b(d, 7) << 24),
                8) + ")"
        elif scs == 0x2:
            v = _idx_sub(d) + " = " + _expedited(d, cmd)
        elif scs == 0x3:
            v = "Schreib-OK " + _idx_sub(d)
        else:
            v = _idx_sub(d) + " [" + can_hex(d) + "]"
        return True, "SDO-Antw Node %d" % node, v
    if fc == 0xC:
        cmd = _b(d, 0)
        ccs = cmd >> 5
        if ccs == 0x2:
            v = "Lesen " + _idx_sub(d)
        elif ccs == 0x1:
            v = "Schreiben " + _idx_sub(d) + " = " + _expedited(d, cmd)
        elif ccs == 0x4:
            v = "Abort " + _idx_sub(d)
        else:
            v = _idx_sub(d) + " [" + can_hex(d) + "]"
        return True, "SDO-Req Node %d" % node, v
    if fc == 0xE:
        st = _b(d, 0) & 0x7F
        v = {0x00: "Boot-up", 0x04: "Stopped", 0x05: "Operational",
             0x7F: "Pre-Operational"}.get(st, "Zustand " + _hx(st, 2))
        return True, "Heartbeat Node %d" % node, v
    return False, "", ""


# ------------------------------------------------------------ CAN-Stand
class CanStand:
    """Tabelle der Prozesswert-Seite: Zeilen in der Reihenfolge des ersten
    Auftretens, je Zeile die Revision der letzten Aenderung."""

    def __init__(self, matrix, ifname):
        self.matrix = matrix
        self.ifname = ifname
        self.lock = threading.Condition()
        self.zeilen = {}            # Schluessel -> dict (insertion order)
        self.rev = 0
        self.frames = 0
        self.status = "CAN: initialisiere …"

    def setze_status(self, text):
        with self.lock:
            self.status = text
            self.rev += 1
            self.lock.notify_all()

    def _zeile(self, key, cid_text, label, wert, einheit, grau, tip):
        z = self.zeilen.get(key)
        if z is None:
            self.zeilen[key] = {"k": key, "id": cid_text, "label": label,
                                "tip": tip, "wert": wert, "einheit": einheit,
                                "grau": grau, "rev": self.rev}
        elif z["wert"] != wert or z["label"] != label:
            z["wert"], z["label"], z["rev"] = wert, label, self.rev

    def frame(self, cid, data):
        with self.lock:
            self.rev += 1
            self.frames += 1
            cid_text = "0x%X" % cid
            sigs = self.matrix.decode(cid, data)
            if sigs:
                for name, label, unit, wert in sigs:
                    self._zeile(name, cid_text, label, wert, unit, False, name)
            else:
                ok, label, value = canopen_describe(cid, data)
                self._zeile("gen:%d" % cid, cid_text,
                            label if ok else "(unbekannt)",
                            value if ok and value else can_hex(data), "", True, "")
            if self.frames % 25 == 0:
                gen = sum(1 for k in self.zeilen if k.startswith("gen:"))
                self.status = ("%s · %d Telegramme · %d Matrix-Signale · %d "
                               "CANopen/roh · Matrix kennt %d" % (
                                   self.ifname, self.frames,
                                   len(self.zeilen) - gen, gen,
                                   self.matrix.count))
            self.lock.notify_all()

    def seit(self, rev):
        """(rev_jetzt, status, geaenderte Zeilen seit rev) - Reihenfolge des
        ersten Auftretens, damit neue Zeilen richtig angehaengt werden."""
        with self.lock:
            return self.rev, self.status, [
                {k: v for k, v in z.items() if k != "rev"}
                for z in self.zeilen.values() if z["rev"] > rev]

    def warte(self, rev, timeout):
        with self.lock:
            self.lock.wait_for(lambda: self.rev != rev, timeout)


def can_leser(stand, ifname):
    """CAN_RAW auf ifname wie CanReader; bei Fehlschlag alle 5 s neu."""
    gemeldet = False
    while True:
        try:
            s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
            s.bind((ifname,))
        except OSError as e:
            if not gemeldet:
                log("CAN %s: %s" % (ifname, os.strerror(e.errno) if e.errno else e))
                stand.setze_status(
                    "CAN: %s nicht verfügbar — Interface hochfahren: "
                    "'ip link set %s up type can bitrate 250000'" % (ifname, ifname))
                gemeldet = True
            time.sleep(5)
            continue
        log("CAN: %s offen" % ifname)
        gemeldet = False
        stand.setze_status("CAN: %s offen · %d Signale in der Matrix · warte "
                           "auf Telegramme …" % (ifname, stand.matrix.count))
        try:
            while True:
                f = s.recv(16)
                if len(f) < 16:
                    continue
                cid, dlc = struct.unpack_from("=IB", f)
                if cid & CAN_ERR_FLAG:
                    continue
                cid = cid & CAN_EFF_MASK if cid & CAN_EFF_FLAG else cid & CAN_SFF_MASK
                stand.frame(cid, f[8:8 + min(dlc, 8)])
        except OSError as e:
            log("CAN %s: Lesen fehlgeschlagen: %s" % (ifname, e))
            s.close()
            time.sleep(1)


def can_quelle(stand, pfad):
    """candump-Log abspielen (Zeilen '(ts) can0 123#DEADBEEF'), endlos."""
    zeilen = []
    for z in open(pfad):
        t = z.split()
        if len(t) >= 3 and "#" in t[2]:
            ident, daten = t[2].split("#", 1)
            zeilen.append((float(t[0].strip("()")), int(ident, 16),
                           bytes.fromhex(daten)))
    stand.setze_status("CAN: %s offen · %d Signale in der Matrix · warte auf "
                       "Telegramme …" % (stand.ifname, stand.matrix.count))
    while zeilen:
        t0, start = zeilen[0][0], time.monotonic()
        for ts, cid, daten in zeilen:
            pause = (ts - t0) - (time.monotonic() - start)
            if pause > 0:
                time.sleep(pause)
            stand.frame(cid, daten)
        time.sleep(0.5)


# ------------------------------------------------------------ idu2-mvb
def dds_abfrage(sock_pfad, seite, neu):
    return idu_anfrage(sock_pfad, {"befehl": "dds", "seite": seite, "neu": neu},
                       DDS_TIMEOUT)


def idu_anfrage(sock_pfad, anfrage, timeout):
    """Eine Anfrage an idu2-mvb wie DdsClient bzw. iduAnfrage in Qt: eine
    JSON-Zeile hin, die erste Zeile zurueck."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(sock_pfad)
        s.sendall(json.dumps(anfrage).encode() + b"\n")
        buf = b""
        while b"\n" not in buf:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        s.close()
    except socket.timeout:
        return {"ok": False, "fehler": "keine Antwort vom Dienst idu2-mvb"}
    except OSError as e:
        return {"ok": False, "fehler": "Dienst idu2-mvb nicht erreichbar (%s) — "
                "läuft er? systemctl status idu2-mvb" % (
                    os.strerror(e.errno) if e.errno else e)}
    try:
        return json.loads(buf.split(b"\n", 1)[0].decode("utf-8"))
    except ValueError as e:
        return {"ok": False, "fehler": "unlesbare Antwort vom Dienst: %s" % e}


# ------------------------------------------------------------ USB-Update
def usb_geraet():
    """Wie findUsbBlockDevice: erstes /sys/block/sd*, dessen Pfad ueber USB
    fuehrt (die CFast haengt an SATA)."""
    for d in sorted(glob.glob("/sys/block/sd*")):
        if "/usb" in os.path.realpath(d):
            return "/dev/" + os.path.basename(d)
    return ""


UPDATE_LOCK = threading.Lock()


def update_einspielen(www):
    if not UPDATE_LOCK.acquire(blocking=False):
        return {"ok": False, "fehler": "Update läuft bereits"}
    try:
        dev = usb_geraet()
        if not dev:
            return {"ok": False, "fehler": "kein USB-Gerät gefunden"}
        part = dev + "1" if os.path.exists(dev + "1") else dev
        os.makedirs(UPDATE_MNT, exist_ok=True)
        subprocess.run(["umount", UPDATE_MNT], capture_output=True)
        r = subprocess.run(["mount", part, UPDATE_MNT], capture_output=True,
                           text=True)
        if r.returncode:
            return {"ok": False, "fehler": "mount: " + (r.stderr.strip() or "Fehler")}
        try:
            archive = sorted(glob.glob(os.path.join(UPDATE_MNT, "*.tar.gz")))
            if not archive:
                return {"ok": False, "fehler": "kein .tar.gz gefunden"}
            erg = webapp_tauschen(archive[0], www)
        finally:
            os.sync()
            subprocess.run(["umount", UPDATE_MNT], capture_output=True)
        return erg
    finally:
        UPDATE_LOCK.release()


def webapp_tauschen(archiv, www):
    """Archiv mit www/index.html neben www entpacken und atomar tauschen."""
    basis = os.path.dirname(os.path.abspath(www))
    tmp = tempfile.mkdtemp(prefix="update-", dir=basis)
    try:
        with tarfile.open(archiv, "r:gz") as t:
            t.extractall(tmp, filter="data")
        treffer = sorted((p for p in glob.glob(os.path.join(tmp, "**", "www",
                                                            "index.html"),
                                               recursive=True)), key=len)
        if not treffer:
            return {"ok": False, "fehler": "www/index.html nicht im Archiv"}
        neu = os.path.join(basis, "www.neu")
        alt = os.path.join(basis, "www.alt")
        shutil.rmtree(neu, ignore_errors=True)
        os.rename(os.path.dirname(treffer[0]), neu)
        shutil.rmtree(alt, ignore_errors=True)
        if os.path.isdir(www):
            os.rename(www, alt)
        os.rename(neu, www)
        log("Update: Web-App aus %s eingespielt" % os.path.basename(archiv))
        return {"ok": True, "meldung": "Update erfolgreich — Seite wird neu "
                "geladen …"}
    except (OSError, tarfile.TarError) as e:
        return {"ok": False, "fehler": "Entpacken: %s" % e}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------------------------ HTTP
class Handler(BaseHTTPRequestHandler):
    server_version = "myboard-webgui-api/" + VERSION
    O = None
    STAND = None

    def log_message(self, fmt, *args):
        pass                            # kein Zugriffsprotokoll im Journal

    def json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return None

    def do_GET(self):
        pfad = urlsplit(self.path).path
        if pfad == "/api/can/strom":
            return self.can_strom()
        if pfad == "/api/can/stand":
            rev, status, zeilen = self.STAND.seit(-1)
            return self.json({"rev": rev, "status": status, "zeilen": zeilen})
        if pfad == "/api/usb":
            return self.json({"stick": bool(usb_geraet())})
        if pfad == "/api/info":
            return self.json({"applikation": "myboard-webgui " + VERSION,
                              "maschine": os.uname().machine,
                              "can": self.O["can"], "status": self.STAND.status})
        if pfad == "/api/softkeys":
            return self.json(idu_anfrage(self.O["dds"], {"befehl": "softkeys"},
                                         SOFTKEY_TIMEOUT))
        if pfad == "/gui" or pfad.startswith("/gui/"):
            return self.datei(pfad[len("/gui"):] or "/")
        self.json({"ok": False, "fehler": "unbekannt: " + pfad}, 404)

    def do_POST(self):
        pfad = urlsplit(self.path).path
        req = self.body_json()
        if req is None or not isinstance(req, dict):
            return self.json({"ok": False, "fehler": "keine gueltige JSON-Anfrage"}, 400)
        if pfad == "/api/dds":
            seite, neu = req.get("seite", 1), req.get("neu", False)
            if not isinstance(seite, int) or not 1 <= seite <= 999 \
                    or not isinstance(neu, bool):
                return self.json({"ok": False, "fehler": "seite 1..999, neu true/false"}, 400)
            return self.json(dds_abfrage(self.O["dds"], seite, neu))
        if pfad == "/api/softkey":
            # welche Signale erlaubt sind, prueft idu2-mvb (Softkey-Liste)
            signal_name = req.get("signal")
            if not isinstance(signal_name, str) or not 0 < len(signal_name) < 40:
                return self.json({"ok": False, "fehler": "signal fehlt"}, 400)
            return self.json(idu_anfrage(
                self.O["dds"], {"befehl": "softkey", "signal": signal_name},
                SOFTKEY_TIMEOUT))
        if pfad == "/api/update":
            return self.json(update_einspielen(self.O["www"]))
        self.json({"ok": False, "fehler": "unbekannt: " + pfad}, 404)

    def can_strom(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        rev, letzte = -1, 0.0
        try:
            while True:
                neu_rev, status, zeilen = self.STAND.seit(rev)
                if neu_rev != rev:
                    paket = {"voll": rev < 0, "status": status, "zeilen": zeilen}
                    self.wfile.write(b"data: " + json.dumps(
                        paket, ensure_ascii=False).encode("utf-8") + b"\n\n")
                    self.wfile.flush()
                    rev, letzte = neu_rev, time.monotonic()
                elif time.monotonic() - letzte > 15:
                    self.wfile.write(b": ping\n\n")    # Verbindung wach halten
                    self.wfile.flush()
                    letzte = time.monotonic()
                time.sleep(SSE_TAKT)
                self.STAND.warte(rev, 15)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def datei(self, rel):
        """Web-App direkt ausliefern (Test ohne Apache)."""
        www = os.path.realpath(self.O["www"])
        p = os.path.realpath(os.path.join(www, rel.lstrip("/")))
        if os.path.isdir(p):
            p = os.path.join(p, "index.html")
        if not (p == www or p.startswith(www + os.sep)) or not os.path.isfile(p):
            return self.json({"ok": False, "fehler": "nicht gefunden"}, 404)
        typ = mimetypes.guess_type(p)[0] or "application/octet-stream"
        if typ.startswith("text/") or typ.endswith("javascript"):
            typ += "; charset=utf-8"
        with open(p, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", typ)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    o = parse_args(sys.argv)
    matrix = CanMatrix()
    if not matrix.laden(o["matrix"]):
        log("Signaltabelle %s nicht lesbar - nur CANopen/Hex" % o["matrix"])
    stand = CanStand(matrix, o["can"])
    Handler.O, Handler.STAND = o, stand
    if o["quelle"]:
        ziel, arg = can_quelle, o["quelle"]
    else:
        ziel, arg = can_leser, o["can"]
    threading.Thread(target=ziel, args=(stand, arg), daemon=True).start()

    srv = ThreadingHTTPServer((o["adresse"], o["port"]), Handler)
    srv.daemon_threads = True
    log("myboard-webgui-api %s auf %s:%d, CAN %s, %d Signale, Web-App %s"
        % (VERSION, o["adresse"], o["port"],
           o["quelle"] or o["can"], matrix.count, o["www"]))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
