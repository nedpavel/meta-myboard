/* MyBoard Web-Oberflaeche - Bedienung wie myboard-gui (Qt).
 *
 * Tasten wie MainWindow::keyPressEvent: C/Esc -> Hauptmenue; 1-7 Seiten;
 * Pfeile: Wagenwahl, auf der DDS-Seite Meldung waehlen/blaettern, auf der
 * Softkey-Seite Taste waehlen (E/Enter drueckt);
 * Update-Seite: 1 = Ja, 2 = Nein. Touch/Maus ueber die Knoepfe.
 * Daten: /api/ (Dienst myboard-webgui-api).
 */
'use strict';

const $ = (sel) => document.querySelector(sel);
const SEITEN = Array.from(document.querySelectorAll('#seiten > .seite'));
const MENUE = 0, DDS = 4, UPDATE = 5, SOFTKEYS = 7;
let seite = MENUE;

/* ------------------------------------------------------------ Skalierung */
/* Wie m_scale in Qt: Basis 640x480, 1024x768 ergibt 1.6. Im Laptop-Fenster
   passt sich die Oberflaeche an, ohne das Seitenverhaeltnis zu aendern. */
function skalieren() {
  const k = Math.min(innerWidth / 640, innerHeight / 480);
  document.documentElement.style.setProperty('--k', k > 0 ? k : 1);
}
skalieren();
addEventListener('resize', skalieren);

/* ------------------------------------------------------------ Kopfzeile */
function zwei(n) { return String(n).padStart(2, '0'); }

function uhr() {
  const t = new Date();
  $('#datum').textContent = zwei(t.getDate()) + '.' + zwei(t.getMonth() + 1) + '.'
                            + zwei(t.getFullYear() % 100);
  $('#uhr').textContent = zwei(t.getHours()) + ':' + zwei(t.getMinutes()) + ':'
                          + zwei(t.getSeconds());
}
uhr();
setInterval(uhr, 1000);

/* ------------------------------------------------------------ Seiten */
function zeigeSeite(i) {
  SEITEN.forEach((s, k) => s.classList.toggle('aktiv', k === i));
  $('#titel').textContent = SEITEN[i].dataset.titel;
  seite = i;
  usbPruefen(i === UPDATE);       /* wie m_usbTimer: nur auf der Update-Seite */
  skAnzeigen(i === SOFTKEYS);     /* wie m_skTimer */
}

document.querySelectorAll('.leiste button[data-seite]').forEach((b) =>
  b.addEventListener('click', () => zeigeSeite(Number(b.dataset.seite))));
document.querySelectorAll('.zurueck').forEach((b) =>
  b.addEventListener('click', () => zeigeSeite(MENUE)));

/* ------------------------------------------------------------ Wagenwahl */
const WAGEN = Array.from(document.querySelectorAll('.wagen-zelle'));
let wagen = 0;

function wagenAnzeigen() {
  WAGEN.forEach((w, i) => w.classList.toggle('gewaehlt', i === wagen));
  $('#halbzug').textContent = (wagen + 1) + '. Halbzug';
  $('#zugnummer').textContent = WAGEN[wagen].querySelector('.wagen-nr').textContent;
}
WAGEN.forEach((w, i) => w.addEventListener('click', () => { wagen = i; wagenAnzeigen(); }));
wagenAnzeigen();

/* ------------------------------------------------------------ CAN live */
/* Der Dienst schickt zuerst die ganze Tabelle, danach hoechstens 4x/s die
   geaenderten Zeilen. Zeilen bleiben in der Reihenfolge ihres ersten
   Auftretens; geaendert werden nur Zellen, deren Text sich aendert. */
const canBody = $('#can-tabelle tbody');
let canZeilen = new Map();

function canZeile(z) {
  let r = canZeilen.get(z.k);
  if (!r) {
    const tr = document.createElement('tr');
    const td = ['eng', '', 'eng', 'eng'].map((cls) => {
      const c = document.createElement('td');
      if (cls) c.className = cls;
      tr.appendChild(c);
      return c;
    });
    td[0].textContent = z.id;
    td[3].textContent = z.einheit;
    if (z.grau) td[1].classList.add('grau');
    if (z.tip) td[1].title = z.tip;
    r = { label: td[1], wert: td[2] };
    canZeilen.set(z.k, r);
    canBody.appendChild(tr);
  }
  if (r.label.textContent !== z.label) r.label.textContent = z.label;
  if (r.wert.textContent !== z.wert) r.wert.textContent = z.wert;
}

function canVerbinden() {
  const es = new EventSource('/api/can/strom');
  es.onmessage = (ev) => {
    const p = JSON.parse(ev.data);
    if (p.voll) {
      canBody.textContent = '';
      canZeilen = new Map();
    }
    p.zeilen.forEach(canZeile);
    $('#can-status').textContent = p.status;
  };
  es.onerror = () => {
    $('#can-status').textContent = 'Verbindung zum Dienst myboard-webgui-api unterbrochen — verbinde neu …';
  };
}
canVerbinden();

/* ------------------------------------------------------------ DDS */
/* Wie die DDS-Seite in Qt (DDS_COLS, startDdsSeite, ddsPfeil,
   onDdsErgebnis): Seiten von DIA1 ueber idu2-mvb, schon geholte Seiten
   liefert der Dienst aus seinem Zwischenspeicher. */
const DDS_COLS = [
  ['uic', 'UIC/Sub'], ['prio', 'P'], ['zaehler', 'Zähl'], ['kommt', 'Kommt'],
  ['geht', 'Geht'], ['status', 'Status'], ['knoten', 'Knoten'], ['text', 'Meldung'],
  ['pos', 'Nr'], ['stoer', 'Stör'], ['ref7', '7xxx'], ['ref8', '8xxx'],
  ['ereignisnr', 'EreignisNr'], ['meldnr', 'MeldNr'],
];
const ddsBody = $('#dds-tabelle tbody');
const ddsKnopf = $('#dds-knopf');
DDS_COLS.forEach(([, kopf]) => {
  const th = document.createElement('th');
  th.textContent = kopf;
  $('#dds-tabelle thead tr').appendChild(th);
});

const dds = { seite: 0, letzte: false, aufwaerts: false, info: '', laeuft: false, zeile: -1 };

function ddsStatus(text, farbe) {
  const s = $('#dds-status');
  s.textContent = text;
  s.style.color = farbe;
}

function ddsZeileWaehlen(i) {
  const rows = ddsBody.rows;
  if (dds.zeile >= 0 && rows[dds.zeile]) rows[dds.zeile].classList.remove('gewaehlt');
  dds.zeile = i;
  const tr = rows[i];
  tr.classList.add('gewaehlt');
  /* nur senkrecht nachfuehren, die seitliche Lage bleibt */
  const rahmen = $('#dds-rahmen');
  const kopf = $('#dds-tabelle thead').offsetHeight;
  if (tr.offsetTop - kopf < rahmen.scrollTop) rahmen.scrollTop = tr.offsetTop - kopf;
  else if (tr.offsetTop + tr.offsetHeight > rahmen.scrollTop + rahmen.clientHeight)
    rahmen.scrollTop = tr.offsetTop + tr.offsetHeight - rahmen.clientHeight;
}

function startDds() { startDdsSeite(1, false, true); }      /* Seite 1 neu von DIA1 */

async function startDdsSeite(nr, aufwaerts, neu) {
  if (dds.laeuft) return;
  dds.laeuft = true;
  dds.aufwaerts = aufwaerts;
  ddsKnopf.disabled = true;
  ddsStatus('Seite ' + nr + ' wird von DIA1 abgefragt …', '#000000');
  const abbruch = new AbortController();
  const timer = setTimeout(() => abbruch.abort(), 60000);   /* wie DdsClient */
  try {
    const r = await fetch('/api/dds', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ seite: nr, neu: neu }), signal: abbruch.signal,
    });
    const antwort = await r.json();
    if (!antwort.ok) ddsFehler(antwort.fehler || 'unbekannter Fehler');
    else ddsErgebnis(antwort);
  } catch (e) {
    ddsFehler(e.name === 'AbortError' ? 'keine Antwort vom Dienst idu2-mvb'
              : 'Dienst myboard-webgui-api nicht erreichbar (' + e.message + ')');
  } finally {
    clearTimeout(timer);
    dds.laeuft = false;
    ddsKnopf.disabled = false;
  }
}

function ddsPfeil(runter) {
  const n = ddsBody.rows.length;
  if (dds.laeuft || n === 0) return;
  const row = dds.zeile;
  if (runter) {
    if (row < n - 1) ddsZeileWaehlen(row + 1);
    else if (dds.letzte)
      ddsStatus(dds.info + '\nEnde der Liste: DIA1 hat keine weiteren Meldungen.', '#B9770E');
    else startDdsSeite(dds.seite + 1, false, false);
  } else {
    if (row > 0) ddsZeileWaehlen(row - 1);
    else if (dds.seite > 1) startDdsSeite(dds.seite - 1, true, false);
  }
}

function zellText(v) {
  if (typeof v === 'number') return Number.isInteger(v) ? String(v) : '0';
  return v == null ? '' : String(v);
}

function ddsErgebnis(antwort) {
  const meld = antwort.meldungen || [];
  const nr = antwort.seite || 1;
  const hinweis = antwort.hinweis || '';

  /* Hinter der letzten Seite: die angezeigte Seite bleibt stehen */
  if (meld.length === 0 && nr > 1) {
    dds.letzte = true;
    ddsStatus(dds.info + '\nEnde der Liste: DIA1 hat keine weiteren Meldungen.'
              + (hinweis ? ' ' + hinweis : ''), '#B9770E');
    return;
  }
  dds.seite = nr;
  dds.letzte = !!antwort.letzte;

  ddsBody.textContent = '';
  dds.zeile = -1;
  meld.forEach((m) => {
    const tr = document.createElement('tr');
    DDS_COLS.forEach(([key]) => {
      const td = document.createElement('td');
      td.textContent = zellText(m[key]);
      if (key === 'status') td.className = m.status === 'aktiv' ? 'aktiv' : 'gegangen';
      tr.appendChild(td);
    });
    tr.addEventListener('click', () => ddsZeileWaehlen(tr.sectionRowIndex));
    ddsBody.appendChild(tr);
  });
  if (meld.length) ddsZeileWaehlen(dds.aufwaerts ? meld.length - 1 : 0);

  const aktive = meld.filter((m) => m.status === 'aktiv').length;
  const bereich = meld.length === 0 ? 'keine Meldungen'
    : 'Meldungen ' + meld[0].pos + '–' + meld[meld.length - 1].pos + ', davon ' + aktive + ' aktiv';
  const herkunft = antwort.zwischengespeichert ? 'aus dem Zwischenspeicher'
    : (Number(antwort.dauer_s) || 0).toFixed(1) + ' s';
  dds.info = 'Seite ' + nr + (dds.letzte ? ' (letzte)' : '') + ' · ' + bereich
    + ' · abgefragt ' + (antwort.zeit || '') + ' · ' + herkunft
    + ' · ▲▼ wählen, ▼ auf der letzten Zeile blättert weiter, 1 lädt neu'
    + ' · Zeiten UTC laut Uhr von DIA1';
  if (hinweis) ddsStatus(dds.info + '\nHinweis: ' + hinweis, '#B9770E');
  else ddsStatus(dds.info, '#2E7D32');
}

function ddsFehler(meldung) {
  /* Die angezeigte Seite bleibt stehen */
  ddsStatus('DDS-Abfrage fehlgeschlagen: ' + meldung, '#C0392B');
}

ddsKnopf.addEventListener('click', startDds);

/* ------------------------------------------------------------ Update */
let usbTimer = null;
let usbDa = false;
let updateLaeuft = false;

function usbHinweis(text, farbe) {
  const h = $('#usb-hinweis');
  h.textContent = text;
  h.style.color = farbe;
}

async function usbStand() {
  if (updateLaeuft) return;
  try {
    usbDa = !!(await (await fetch('/api/usb', { cache: 'no-store' })).json()).stick;
  } catch (e) {
    usbDa = false;
  }
  if (updateLaeuft || seite !== UPDATE) return;
  $('.ja-nein').classList.toggle('stick', usbDa);
  if (usbDa) usbHinweis('USB-Stick erkannt — Taste 1 = Ja, Taste 2 = Nein.', '#2E7D32');
  else usbHinweis('Kein USB-Stick angeschlossen — nur C (zurück).', '#B0B0B0');
}

function usbPruefen(an) {
  clearInterval(usbTimer);
  usbTimer = null;
  if (an) {
    usbStand();
    usbTimer = setInterval(usbStand, 1000);
  }
}

async function updateStarten() {
  if (!usbDa || updateLaeuft) return;
  updateLaeuft = true;
  clearInterval(usbTimer);                /* waehrend des Updates nicht pruefen */
  usbHinweis('Update läuft … bitte warten.', '#000000');
  let antwort;
  try {
    antwort = await (await fetch('/api/update', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}',
    })).json();
  } catch (e) {
    antwort = { ok: false, fehler: 'Dienst myboard-webgui-api nicht erreichbar' };
  }
  updateLaeuft = false;
  if (antwort.ok) {
    usbHinweis(antwort.meldung || 'Update erfolgreich — Seite wird neu geladen …', '#2E7D32');
    setTimeout(() => location.reload(), 1500);
  } else {
    usbHinweis('Update fehlgeschlagen: ' + (antwort.fehler || 'Fehler'), '#C0392B');
    if (seite === UPDATE) usbTimer = setInterval(usbStand, 1000);
  }
}

$('#ja').addEventListener('click', updateStarten);
$('#nein').addEventListener('click', () => { if (usbDa) zeigeSeite(MENUE); });

/* ------------------------------------------------------------ Softkeys */
/* Wie die Softkey-Seite in Qt: welche Softkeys es gibt, sagt idu2-mvb
   (/api/softkeys, je nach Rolle IDU1/IDU2); ein Druck (/api/softkey) ist
   dort ein Puls auf dem Source-Port. Abgefragt wird nur, solange die Seite
   offen ist. */
const sk = { timer: null, laeuft: false, signatur: '', spalten: [],
             spalte: 0, zeile: 0, meldungBis: 0, dauer: 1 };

function skStatus(text, farbe) {
  const s = $('#sk-status');
  s.textContent = text;
  s.style.color = farbe;
}

async function skAbfragen() {
  if (sk.laeuft) return;                  /* vorige Anfrage noch unterwegs */
  sk.laeuft = true;
  let a;
  try {
    a = await (await fetch('/api/softkeys', { cache: 'no-store' })).json();
  } catch (e) {
    a = { ok: false, fehler: 'Dienst myboard-webgui-api nicht erreichbar' };
  }
  sk.laeuft = false;
  if (seite === SOFTKEYS) skStand(a);
}

function skAufbauen(gruppen, rueck) {
  const r = $('#sk-rueck');
  r.textContent = '';
  r.style.gridTemplateColumns = 'repeat(' + Math.max(1, Math.ceil(rueck.length / 2)) + ', 1fr)';
  rueck.forEach((x) => {
    const d = document.createElement('div');
    d.textContent = x.text;
    d.dataset.signal = x.signal;
    r.appendChild(d);
  });
  if (!rueck.length) {
    const d = document.createElement('div');
    d.className = 'leer';
    d.textContent = 'Keine Rückmeldungen (Prozessdaten in idu2-mvb aus)';
    r.appendChild(d);
  }
  const g = $('#sk-gruppen');
  g.textContent = '';
  sk.spalten = [];
  gruppen.forEach((gr, c) => {
    const sp = document.createElement('div');
    sp.className = 'sk-spalte';
    const h = document.createElement('h3');
    h.textContent = gr.name;
    sp.appendChild(h);
    const knoepfe = [];
    gr.tasten.forEach((t, z) => {
      const b = document.createElement('button');
      b.textContent = t.text;
      b.dataset.signal = t.signal;
      b.addEventListener('click', () => { skWaehlen(c, z); skDruecken(t.signal); });
      sp.appendChild(b);
      knoepfe.push(b);
    });
    g.appendChild(sp);
    sk.spalten.push(knoepfe);
  });
  skWaehlen(Math.min(sk.spalte, sk.spalten.length - 1), sk.zeile);
}

function skStand(a) {
  if (!a.ok) {
    skStatus('Softkeys nicht verfügbar: ' + (a.fehler || 'Fehler'), '#C0392B');
    return;
  }
  const gruppen = a.gruppen || [], rueck = a.rueckmeldungen || [];
  sk.dauer = a.dauer || 1;
  /* neu aufbauen nur, wenn sich die Tasten aendern (andere Rolle) */
  const sig = [a.rolle].concat(...gruppen.map((g) => g.tasten.map((t) => t.signal)),
                               rueck.map((x) => 'r:' + x.signal)).join(',');
  if (sig !== sk.signatur) {
    sk.signatur = sig;
    skAufbauen(gruppen, rueck);
  }
  /* Knoepfe blau, solange der Puls laeuft */
  const knopf = new Map(sk.spalten.flat().map((b) => [b.dataset.signal, b]));
  gruppen.forEach((g) => g.tasten.forEach((t) => {
    const b = knopf.get(t.signal);
    if (b) b.classList.toggle('aktiv', t.wert === 1);
  }));
  /* Rueckmeldungen: gruen = 1, gestrichelt = Port veraltet */
  rueck.forEach((x) => {
    const d = $('#sk-rueck').querySelector('[data-signal="' + CSS.escape(x.signal) + '"]');
    if (!d) return;
    const alt = !x.frisch || x.wert == null;
    d.classList.toggle('alt', alt);
    d.classList.toggle('an', !alt && x.wert === 1);
  });
  if (Date.now() < sk.meldungBis) return;   /* Meldung zum letzten Druck steht */
  if (!gruppen.length) skStatus('Keine Softkeys in der Rolle ' + a.rolle + '.', '#B9770E');
  else skStatus('Rolle ' + a.rolle + ' · jeder Druck ist ein Puls von ' + sk.dauer
                + ' s auf dem MVB · gestrichelte Rückmeldung = keine aktuellen Daten', '#555555');
}

async function skDruecken(signal) {
  const b = sk.spalten.flat().find((x) => x.dataset.signal === signal);
  const text = b ? b.textContent.replace(/\n/g, ' ') : signal;
  let a;
  try {
    a = await (await fetch('/api/softkey', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ signal: signal }),
    })).json();
  } catch (e) {
    a = { ok: false, fehler: 'Dienst myboard-webgui-api nicht erreichbar' };
  }
  if (a.ok) skStatus(text + ': Puls ' + a.dauer + ' s gesendet', '#2E7D32');
  else skStatus(text + ' nicht gesendet: ' + (a.fehler || 'Fehler'), '#C0392B');
  sk.meldungBis = Date.now() + 4000;
  skAbfragen();                           /* Knopf gleich blau zeigen */
}

function skWaehlen(spalte, zeile) {
  if (!sk.spalten.length || spalte < 0) return;
  zeile = Math.max(0, Math.min(zeile, sk.spalten[spalte].length - 1));
  sk.spalten.forEach((sp, c) => sp.forEach((b, z) =>
    b.classList.toggle('gewaehlt', c === spalte && z === zeile)));
  sk.spalte = spalte;
  sk.zeile = zeile;
}

function skPfeil(dx, dy) {
  if (!sk.spalten.length) return;
  skWaehlen(Math.max(0, Math.min(sk.spalte + dx, sk.spalten.length - 1)), sk.zeile + dy);
}

function skAnzeigen(an) {
  clearInterval(sk.timer);
  sk.timer = null;
  if (an) {
    skAbfragen();
    sk.timer = setInterval(skAbfragen, 400);
  }
}

/* ------------------------------------------------------------ Info */
fetch('/api/info', { cache: 'no-store' }).then((r) => r.json()).then((i) => {
  $('#info-app').textContent = i.applikation;
  $('#info-can').textContent = i.can + ' · Matrix + CANopen-Decoder';
}).catch(() => {});

/* ------------------------------------------------------------ Tasten */
document.addEventListener('keydown', (ev) => {
  if (ev.ctrlKey || ev.altKey || ev.metaKey) return;
  const k = ev.key;
  let erledigt = true;

  if (k === 'c' || k === 'C' || k === 'Escape') {
    zeigeSeite(MENUE);
  } else if (seite === UPDATE) {
    if (k === '1') updateStarten();
    else if (k === '2') { if (usbDa) zeigeSeite(MENUE); }
    /* sonst nichts (nur C zurueck) */
  } else if (seite === DDS && k === '1') {
    startDds();
  } else if (seite === DDS && (k === 'ArrowDown' || k === 'ArrowUp')) {
    ddsPfeil(k === 'ArrowDown');
  } else if (seite === SOFTKEYS && k.startsWith('Arrow')) {
    skPfeil(k === 'ArrowLeft' ? -1 : k === 'ArrowRight' ? 1 : 0,
            k === 'ArrowUp' ? -1 : k === 'ArrowDown' ? 1 : 0);
  } else if (seite === SOFTKEYS && k === 'Enter') {   /* Taste E der Front */
    const b = (sk.spalten[sk.spalte] || [])[sk.zeile];
    if (b) skDruecken(b.dataset.signal);
  } else if (k >= '1' && k <= '7' && k.length === 1) {
    zeigeSeite(Number(k));
  } else if (k === 'ArrowUp') {           /* Wagen-Auswahl nach rechts */
    if (wagen < WAGEN.length - 1) { wagen++; wagenAnzeigen(); }
  } else if (k === 'ArrowDown') {         /* Wagen-Auswahl nach links */
    if (wagen > 0) { wagen--; wagenAnzeigen(); }
  } else {
    erledigt = false;
  }
  if (erledigt) ev.preventDefault();
});
