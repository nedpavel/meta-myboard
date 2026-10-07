// SPDX-License-Identifier: GPL-2.0
/*
 * tm_replay - spielt die Treiberlogik gegen einen echten Abzug des
 * Traffic Memory ab.
 *
 * Der Abzug stammt vom laufenden Geraet unter dem Herstellertreiber und
 * enthaelt 74 konfigurierte Ports, die drei Message-Ringe und die
 * Service Area. Damit laesst sich alles pruefen, was nur liest oder
 * rechnet - ohne Hardware und ohne Reboot.
 */

#define __KERNEL__ 1

#include <linux/kernel.h>
#include <stdarg.h>

u8 fake_tm[FAKE_TM_SIZE];
struct fake_task fake_current = { (void *)1, (void *)1 };
unsigned long fake_io_writes;
int (*fake_ioread_hook)(unsigned long off, u16 *val);

/*
 * Ohne Pfadangabe, damit die Quelle ueber -I gefunden wird: lokal aus
 * ../recipes-kernel/pixy-mvblli/files (siehe Makefile), im Yocto-Bau aus
 * dem Auspackverzeichnis. Eingebunden wird die ganze .c-Datei, so sind
 * auch ihre static-Funktionen erreichbar.
 */
#include "pixy-mvblli.c"

static int fails, checks;

static void check(int cond, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (cond)
		return;
	fails++;
	fprintf(stderr, "FEHLER: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void load(const char *path, unsigned long off)
{
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f) {
		perror(path);
		exit(2);
	}
	n = fread(fake_tm + off, 1, FAKE_TM_SIZE - off, f);
	fclose(f);
	printf("  %s -> TM+0x%05lx, %zu Byte\n", path, off, n);
}

static struct mvblli_dev dev;

static void setup_dev(void)
{
	static const u32 sa_offs[TM_OFFSET_COUNT] = TM_SERVICE_OFFSETS;
	static const u32 la_pit[TM_OFFSET_COUNT] = TM_LA_PIT_OFFSETS;
	static const u32 la_pcs[TM_OFFSET_COUNT] = TM_LA_PCS_OFFSETS;
	static const u32 la_dat[TM_OFFSET_COUNT] = TM_LA_DATA_OFFSETS;
	static const u32 da_pit[TM_OFFSET_COUNT] = TM_DA_PIT_OFFSETS;
	static const u32 pit_sz[TM_OFFSET_COUNT] = TM_PIT_BYTE_SIZES;

	memset(&dev, 0, sizeof(dev));
	dev.mcm = 3;
	dev.p_tm = fake_tm;
	dev.p_sa = fake_tm + sa_offs[3];
	dev.off_la_pit = la_pit[3];
	dev.off_da_pit = da_pit[3];
	dev.off_la_pcs = la_pcs[3];
	dev.off_la_data = la_dat[3];
	dev.pit_bytes = pit_sz[3];
	dev.pit_type = 1;
	dev.prt_indx_max = 0xfff;
	dev.prt_addr_max = TM_PORT_COUNT - 1;
	dev.ts_type = 0;
	dev.rcv_elems = MVBLLI_RCV_RING_ELEMS;
	dev.rcv_ring = calloc(dev.rcv_elems, MVB_MSG_FRAME_SIZE);
}

/* ---------------------------------------------------------- Test 1 */
/*
 * Jeder im Abzug konfigurierte Port wird mit apd_get_port() gelesen.
 * Geprueft wird gegen eine unabhaengig gerechnete Referenz: Dock-Index
 * aus der PIT, F-Code und Typ aus dem PCS, Daten aus der von VP
 * bezeichneten Seite.
 */
static void test_read_all_ports(void)
{
	int ports = 0, src = 0, snk = 0, dti_nonzero = 0;
	int p;

	printf("\n--- Test 1: alle konfigurierten Ports lesen ---\n");

	for (p = 0; p < TM_PORT_COUNT; p++) {
		u16 idx = lp_port_index(&dev, p);
		u16 w0, w1, tack = 0, data[16], ref[16];
		u32 pcs, ref_off;
		int len, rc, i;

		if (!idx)
			continue;
		ports++;

		pcs = lp_pcs_off(&dev, idx);
		w0 = tm_r16(&dev, pcs);
		w1 = tm_r16(&dev, pcs + 2);
		len = 2 << (w0 >> TM_PCS_FCODE_OFF);

		if (((w0 >> TM_PCS_TYPE_OFF) & 3) == TM_PCS_TYPE_SRC)
			src++;
		else
			snk++;
		if ((w0 >> TM_PCS_DTI_OFF) & 7)
			dti_nonzero++;

		/* unabhaengige Referenz, ohne die Treiberfunktionen */
		ref_off = dev.off_la_data + (idx >> 2) * 64 +
			  ((w1 & TM_PCS_VP_MSK) ? 32 : 0) + (idx & 3) * 8;
		for (i = 0; i < len / 2; i++)
			ref[i] = fake_tm[ref_off + i * 2] |
				 (fake_tm[ref_off + i * 2 + 1] << 8);

		rc = apd_get_port(&dev, p, &tack, data, len);
		check(rc == 0, "Port %d: apd_get_port liefert %d", p, rc);
		check(memcmp(data, ref, len) == 0,
		      "Port %d (Idx %u): gelesene Daten weichen ab", p, idx);
		check(tack == tm_r16(&dev, pcs + 4),
		      "Port %d: tack falsch", p);

		/* falsche Laenge muss abgewiesen werden */
		rc = apd_get_port(&dev, p, &tack, data, len == 32 ? 16 : 32);
		check(rc == 3, "Port %d: falsche Laenge liefert %d statt 3",
		      p, rc);
	}

	printf("  %d Ports gelesen: %d Senken, %d Quellen\n", ports, snk, src);
	check(ports == 74, "74 Ports erwartet, %d gefunden", ports);
	check(src == 3, "3 Quellports erwartet, %d gefunden", src);
	check(dti_nonzero == 0, "%d Ports mit dti != 0", dti_nonzero);

	/* unbekannter Port */
	{
		u16 tack, data[16];

		check(apd_get_port(&dev, 3999, &tack, data, 4) == 8,
		      "unbekannter Port liefert nicht 8");
	}
}

/* ---------------------------------------------------------- Test 2 */
/* Die drei Message-Ringe im Abzug: genau ein Waechter je Ring. */
static void test_rings(void)
{
	static const struct { const char *n; u32 base; int llrs; u32 qdt; }
	r[] = {
		{ "xmit0", MD_TQ0_OFFSET, MD_TQ0_LLRS, SA_QDT + 0 },
		{ "xmit1", MD_TQ1_OFFSET, MD_TQ1_LLRS, SA_QDT + 2 },
		{ "rcve",  MD_RQ_OFFSET,  MD_RQ_LLRS,  SA_QDT + 4 },
	};
	unsigned int k;

	printf("\n--- Test 2: Ringstruktur im Abzug ---\n");

	for (k = 0; k < ARRAY_SIZE(r); k++) {
		u16 start = off_to_p16(r[k].base);
		u16 cur = start;
		int n = 0, guards = 0;
		u16 guard_at = 0;

		do {
			if (tm_r16(&dev, p16_to_off(cur)) == 0) {
				guards++;
				guard_at = cur;
			}
			cur = tm_r16(&dev, p16_to_off(cur) + 2);
			n++;
		} while (cur != start && n <= r[k].llrs + 1);

		printf("  %-6s %3d LLR, Waechter bei 0x%04x, QDT 0x%04x\n",
		       r[k].n, n, guard_at, sa_r16(&dev, r[k].qdt));
		check(n == r[k].llrs, "%s: %d LLR statt %d",
		      r[k].n, n, r[k].llrs);
		check(guards == 1, "%s: %d Waechter statt genau einem",
		      r[k].n, guards);
	}
}

/* ---------------------------------------------------------- Test 3 */
/* Der Empfangsdispatcher darf im Abzug nichts finden: Queue war leer. */
static void test_dispatcher_empty(void)
{
	unsigned long before = fake_io_writes;

	printf("\n--- Test 3: Empfangsdispatcher auf leerer Queue ---\n");

	dev.p16_rq = 0;
	{
		/* Softwareposition = Waechter suchen */
		u16 start = off_to_p16(MD_RQ_OFFSET), cur = start;

		do {
			if (tm_r16(&dev, p16_to_off(cur)) == 0) {
				dev.p16_rq = cur;
				break;
			}
			cur = tm_r16(&dev, p16_to_off(cur) + 2);
		} while (cur != start);
	}
	check(dev.p16_rq != 0, "kein Waechter im Empfangsring gefunden");

	mvb_md_dispatcher(&dev);

	check(dev.rcv_filled == 0,
	      "Dispatcher hat %d Frames geholt, erwartet 0", dev.rcv_filled);
	check(fake_io_writes == before,
	      "Dispatcher hat %lu Schreibzugriffe gemacht, erwartet 0",
	      fake_io_writes - before);
	printf("  Queue korrekt als leer erkannt\n");
}

/* ---------------------------------------------------------- Test 4 */
/*
 * Der Allokator aus PD_CONF wird auf einen leeren Traffic Memory
 * losgelassen, mit genau der Portliste, die im Abzug steht. Danach muss
 * die PIT dieselben Index-MENGEN je Groessenklasse enthalten wie das
 * Original, und STSR muss 0x10DB ergeben.
 */
static int cmp_u16(const void *a, const void *b)
{
	return (int)*(const u16 *)a - (int)*(const u16 *)b;
}

static void test_allocator(void)
{
	PixyMvblliConfigLpPrt list[128];
	u16 orig_idx[128], new_idx[128];
	u16 sizes[128];
	int count = 0, p, i;
	u8 *saved;

	printf("\n--- Test 4: Dock-Index-Vergabe gegen die Messung ---\n");

	/* Portliste aus dem Abzug gewinnen */
	for (p = 0; p < TM_PORT_COUNT && count < 128; p++) {
		u16 idx = lp_port_index(&dev, p);
		u16 w0;

		if (!idx)
			continue;
		w0 = tm_r16(&dev, lp_pcs_off(&dev, idx));
		list[count].prt_addr = p;
		list[count].size = 2 << (w0 >> TM_PCS_FCODE_OFF);
		list[count].type = (w0 >> TM_PCS_TYPE_OFF) & 3;
		orig_idx[count] = idx;
		sizes[count] = list[count].size;
		count++;
	}
	printf("  %d Ports aus dem Abzug uebernommen\n", count);

	/* Traffic Memory sichern und leeren */
	saved = malloc(FAKE_TM_SIZE);
	memcpy(saved, fake_tm, FAKE_TM_SIZE);
	memset(fake_tm, 0, FAKE_TM_SIZE);
	dev.all_tacks = calloc(TM_PORT_COUNT, sizeof(u16));

	check(mvb_conf_ports(&dev, list, count) == 0,
	      "mvb_conf_ports schlaegt fehl");

	printf("  STSR nach der Konfiguration: 0x%04X (erwartet 0x10DB)\n",
	       sa_r16(&dev, MVBC_STSR));
	check(sa_r16(&dev, MVBC_STSR) == 0x10DB,
	      "STSR ist 0x%04X statt 0x10DB", sa_r16(&dev, MVBC_STSR));

	for (i = 0; i < count; i++)
		new_idx[i] = lp_port_index(&dev, list[i].prt_addr);

	/* je Groessenklasse die Indexmengen vergleichen */
	{
		static const u16 klass[] = { 32, 16, 8, 4, 2 };
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(klass); k++) {
			u16 a[128], b[128];
			int na = 0, nb = 0, j;

			for (j = 0; j < count; j++)
				if (sizes[j] == klass[k]) {
					a[na++] = orig_idx[j];
					b[nb++] = new_idx[j];
				}
			if (!na)
				continue;
			qsort(a, na, sizeof(u16), cmp_u16);
			qsort(b, nb, sizeof(u16), cmp_u16);
			check(memcmp(a, b, na * sizeof(u16)) == 0,
			      "%2u-Byte-Klasse: Indexmenge weicht ab "
			      "(original %u..%u, neu %u..%u)",
			      klass[k], a[0], a[na - 1], b[0], b[nb - 1]);
			printf("  %2u B: %2d Ports, Indizes %3u..%3u  %s\n",
			       klass[k], na, b[0], b[nb - 1],
			       memcmp(a, b, na * sizeof(u16)) ? "ABWEICHUNG" : "ok");
		}
	}

	/*
	 * PCS-Woerter gegen das Original. Nach PD_CONF traegt nur eine Senke
	 * ihren Typ; eine Quelle bleibt passiv, bis sie das erste Mal
	 * beschrieben wird (lp_ts_open_port / apd_put_port).
	 */
	{
		int diff = 0, passive = 0, first_src = -1;
		u16 buf[16] = { 0 };

		for (i = 0; i < count; i++) {
			u16 w0 = tm_r16(&dev, lp_pcs_off(&dev, new_idx[i]));
			u16 exp = ((u16)lp_len_2_fcode(list[i].size)
				   << TM_PCS_FCODE_OFF) |
				  ((list[i].type == TM_PCS_TYPE_SNK)
				   ? (TM_PCS_TYPE_SNK << TM_PCS_TYPE_OFF) : 0);

			if (w0 != exp)
				diff++;
			if (list[i].type == TM_PCS_TYPE_SRC) {
				passive += !(w0 & TM_PCS_TYPE_MSK);
				if (first_src < 0)
					first_src = i;
			}
		}
		check(diff == 0, "%d PCS-Woerter weichen ab", diff);
		check(passive == 3, "%d statt 3 Quellen passiv", passive);
		printf("  Quellen nach PD_CONF passiv: %d von 3\n", passive);

		if (first_src >= 0) {
			u16 port = list[first_src].prt_addr;
			u32 pcs = lp_pcs_off(&dev, new_idx[first_src]);

			check(apd_put_port(&dev, port, buf,
					   list[first_src].size) == 0,
			      "erstes Schreiben auf Quelle %u abgelehnt", port);
			check(((tm_r16(&dev, pcs) >> TM_PCS_TYPE_OFF) & 3) ==
			      TM_PCS_TYPE_SRC,
			      "Quelle %u nach dem Schreiben nicht aktiv", port);
			printf("  Quelle %u nach erstem write(): PCS 0x%04x\n",
			       port, tm_r16(&dev, pcs));
		}
		printf("  PCS-Wort 0 fuer alle %d Ports korrekt\n", count);
	}

	free(dev.all_tacks);
	dev.all_tacks = NULL;
	memcpy(fake_tm, saved, FAKE_TM_SIZE);
	free(saved);
}

/* ---------------------------------------------------------- Test 5 */
/*
 * Schreiben und Zuruecklesen eines Quellports: die Daten muessen in der
 * zuvor unsichtbaren Seite landen, VP muss umschalten, und der Lesepfad
 * muss danach genau das liefern.
 */
static void test_pd_roundtrip(void)
{
	u16 pat[16], back[16], tack;
	int p, src_port = -1, i;
	u16 idx, vp_before, vp_after, len = 0;
	u32 pcs;

	printf("\n--- Test 5: Prozessdaten schreiben und zuruecklesen ---\n");

	for (p = 0; p < TM_PORT_COUNT; p++) {
		idx = lp_port_index(&dev, p);
		if (!idx)
			continue;
		if (((tm_r16(&dev, lp_pcs_off(&dev, idx)) >> TM_PCS_TYPE_OFF)
		     & 3) == TM_PCS_TYPE_SRC) {
			src_port = p;
			break;
		}
	}
	check(src_port >= 0, "kein Quellport im Abzug gefunden");
	if (src_port < 0)
		return;

	idx = lp_port_index(&dev, src_port);
	pcs = lp_pcs_off(&dev, idx);
	len = 2 << (tm_r16(&dev, pcs) >> TM_PCS_FCODE_OFF);
	vp_before = tm_r16(&dev, pcs + 2) & TM_PCS_VP_MSK;

	for (i = 0; i < 16; i++)
		pat[i] = 0xA5A5 ^ (i * 0x1111);

	check(apd_put_port(&dev, src_port, pat, len) == 0,
	      "apd_put_port schlaegt fehl");

	vp_after = tm_r16(&dev, pcs + 2) & TM_PCS_VP_MSK;
	check(vp_before != vp_after, "VP hat nicht umgeschaltet");

	memset(back, 0, sizeof(back));
	check(apd_get_port(&dev, src_port, &tack, back, len) == 0,
	      "apd_get_port nach dem Schreiben schlaegt fehl");
	check(memcmp(pat, back, len) == 0,
	      "zurueckgelesene Daten weichen ab");

	printf("  Port %d (Idx %u, %u B): VP %u -> %u, Rundlauf ok\n",
	       src_port, idx, len, !!vp_before, !!vp_after);

	/* Schreiben auf eine Senke muss abgewiesen werden */
	for (p = 0; p < TM_PORT_COUNT; p++) {
		idx = lp_port_index(&dev, p);
		if (!idx)
			continue;
		if (((tm_r16(&dev, lp_pcs_off(&dev, idx)) >> TM_PCS_TYPE_OFF)
		     & 3) == TM_PCS_TYPE_SNK) {
			int rc = apd_put_port(&dev, p, pat,
					      2 << (tm_r16(&dev, lp_pcs_off(&dev, idx))
						    >> TM_PCS_FCODE_OFF));

			/* 9 wie apd_put_port im Original, nach aussen EIO */
			check(rc == 9, "Schreiben auf Senke %d liefert %d "
			      "statt 9", p, rc);
			printf("  Schreiben auf Senke %d korrekt abgewiesen\n", p);
			break;
		}
	}
}

/* ---------------------------------------------------------- Test 6 */
/*
 * Ein frisch aufgebauter Sendering, ein Frame hineingelegt, und der
 * Link_Header gegen die Bytes aus dem echten Bustrace gehalten.
 */
static void test_md_send(void)
{
	u16 frame[16];
	u32 buf_off;
	u16 guard, qdt;
	int i;

	printf("\n--- Test 6: Message senden, Link_Header pruefen ---\n");

	memset(fake_tm + MD_TQ0_OFFSET, 0, 0x4000);
	dev.p16_tq0 = mvb_md_install_q(&dev, MD_TQ0_OFFSET, MD_TQ0_LLRS);
	dev.p16_tq1 = mvb_md_install_q(&dev, MD_TQ1_OFFSET, MD_TQ1_LLRS);
	sa_w16(&dev, SA_QDT + 0, 0);
	sa_w16(&dev, SA_QDT + 2, 0);
	dev.q_tq_priority = 0;

	/* Waechter des frisch gebauten Rings */
	check(tm_r16(&dev, p16_to_off(dev.p16_tq1)) == 0,
	      "frischer Ring: erstes LLR traegt einen Datenzeiger");

	/* eigene Adresse 240, Ziel 6 - wie im aufgezeichneten Verkehr */
	sa_w16(&dev, MVBC_DAOR, 240);
	for (i = 0; i < 16; i++)
		frame[i] = 0xDEAD;

	check(mvb_sndp(&dev, 6, 0, frame) == 0, "mvb_sndp schlaegt fehl");

	/* der Puffer haengt jetzt am vorherigen LLR */
	buf_off = p16_to_off(tm_r16(&dev, p16_to_off(
			off_to_p16(MD_TQ1_OFFSET))));
	printf("  Frame liegt bei TM+0x%05X: %02x %02x %02x %02x\n", buf_off,
	       fake_tm[buf_off], fake_tm[buf_off + 1],
	       fake_tm[buf_off + 2], fake_tm[buf_off + 3]);

	/* Erwartung aus dem Bustrace: "10 06 80 f0" */
	check(fake_tm[buf_off + 0] == 0x10, "mode|DD hoch: 0x%02x statt 0x10",
	      fake_tm[buf_off + 0]);
	check(fake_tm[buf_off + 1] == 0x06, "DD niedrig: 0x%02x statt 0x06",
	      fake_tm[buf_off + 1]);
	check(fake_tm[buf_off + 2] == 0x80, "proto|SD hoch: 0x%02x statt 0x80",
	      fake_tm[buf_off + 2]);
	check(fake_tm[buf_off + 3] == 0xf0, "SD niedrig: 0x%02x statt 0xf0",
	      fake_tm[buf_off + 3]);

	/* Nutzdaten ab Byte 4 unveraendert */
	for (i = 2; i < 16; i++)
		check(ioread16(fake_tm + buf_off + i * 2) == 0xDEAD,
		      "Nutzwort %d veraendert", i);

	/* Broadcast: mode muss 0xF werden */
	check(mvb_sndp(&dev, 0xfff, 0, frame) == 0, "Broadcast schlaegt fehl");
	{
		u32 b2 = p16_to_off(tm_r16(&dev, p16_to_off(
				off_to_p16(MD_TQ1_OFFSET) + 1)));

		printf("  Broadcast-Header: %02x %02x\n",
		       fake_tm[b2], fake_tm[b2 + 1]);
		check(fake_tm[b2] == 0xff && fake_tm[b2 + 1] == 0xff,
		      "Broadcast-Header ist %02x %02x statt ff ff",
		      fake_tm[b2], fake_tm[b2 + 1]);
	}

	/* Waechter und QDT muessen zueinander passen */
	guard = 0;
	{
		u16 start = off_to_p16(MD_TQ1_OFFSET), cur = start;
		int n = 0;

		do {
			if (tm_r16(&dev, p16_to_off(cur)) == 0) {
				guard = cur;
				n++;
			}
			cur = tm_r16(&dev, p16_to_off(cur) + 2);
		} while (cur != start);
		check(n == 1, "nach zwei Sendungen %d Waechter im Ring", n);
	}
	qdt = sa_r16(&dev, SA_QDT + 2);
	printf("  Waechter 0x%04x, QDT 0x%04x, Softwareposition 0x%04x\n",
	       guard, qdt, dev.p16_tq1);
	check(guard == dev.p16_tq1,
	      "Waechter 0x%04x != Softwareposition 0x%04x", guard, dev.p16_tq1);
}

/* ---------------------------------------------------------- Test 7 */
/* Empfang: ein Frame von Hand in den Ring legen und abholen lassen. */
static void test_md_receive(void)
{
	u8 payload[MVB_MSG_FRAME_SIZE];
	u16 start, next_p16, buf_p16;
	u32 buf_off;
	int i;

	printf("\n--- Test 7: Message empfangen ---\n");

	memset(fake_tm + MD_RQ_OFFSET, 0, 0x4000);
	dev.p16_rq = mvb_md_install_q(&dev, MD_RQ_OFFSET, MD_RQ_LLRS);
	dev.rcv_filled = dev.rcv_rd = dev.rcv_wr = 0;

	start = dev.p16_rq;
	next_p16 = tm_r16(&dev, p16_to_off(start) + 2);
	buf_p16 = tm_r16(&dev, p16_to_off(next_p16));
	buf_off = p16_to_off(buf_p16);

	/* Frame wie aus dem Trace: Ziel 240, Quelle 6, PT = 1000b */
	for (i = 0; i < MVB_MSG_FRAME_SIZE; i++)
		payload[i] = 0x40 + i;
	payload[0] = 0x10; payload[1] = 0xf0;
	payload[2] = 0x80; payload[3] = 0x06;
	memcpy(fake_tm + buf_off, payload, sizeof(payload));

	/* die Hardware haette den QDT weitergeschaltet */
	sa_w16(&dev, SA_QDT + 4, tm_r16(&dev, p16_to_off(next_p16) + 2));

	mvb_md_dispatcher(&dev);

	check(dev.rcv_filled == 1, "%d Frames geholt, erwartet 1",
	      dev.rcv_filled);
	check(memcmp(dev.rcv_ring, payload, sizeof(payload)) == 0,
	      "empfangenes Frame weicht ab");
	check(tm_r16(&dev, p16_to_off(next_p16)) == 0,
	      "Waechter ist nicht weitergerueckt");
	check(tm_r16(&dev, p16_to_off(start)) == buf_p16,
	      "Puffer wurde nicht ans vorherige LLR gehaengt");
	check(dev.p16_rq == next_p16, "Softwareposition nicht nachgezogen");
	printf("  Frame korrekt uebernommen, Waechter gewandert\n");

	/* Frame mit falschem Protokolltyp muss verworfen werden */
	dev.rcv_filled = dev.rcv_rd = dev.rcv_wr = 0;
	next_p16 = tm_r16(&dev, p16_to_off(dev.p16_rq) + 2);
	buf_p16 = tm_r16(&dev, p16_to_off(next_p16));
	buf_off = p16_to_off(buf_p16);
	payload[2] = 0x90;		/* PT = 1001b, nicht TCN-RTP */
	memcpy(fake_tm + buf_off, payload, sizeof(payload));
	sa_w16(&dev, SA_QDT + 4, tm_r16(&dev, p16_to_off(next_p16) + 2));

	mvb_md_dispatcher(&dev);
	check(dev.rcv_filled == 0,
	      "Frame mit falschem Protokolltyp wurde angenommen");
	printf("  Fremdes Protokoll korrekt verworfen\n");

	/*
	 * Zwei Frames liegen an: wie im Original holt jeder Aufruf genau
	 * eines ab, das zweite bleibt im Ring des Controllers.
	 */
	{
		u16 p = dev.p16_rq, n1, n2;

		dev.rcv_filled = dev.rcv_rd = dev.rcv_wr = 0;
		payload[2] = 0x80;
		n1 = tm_r16(&dev, p16_to_off(p) + 2);
		n2 = tm_r16(&dev, p16_to_off(n1) + 2);
		memcpy(fake_tm + p16_to_off(tm_r16(&dev, p16_to_off(n1))),
		       payload, sizeof(payload));
		memcpy(fake_tm + p16_to_off(tm_r16(&dev, p16_to_off(n2))),
		       payload, sizeof(payload));
		sa_w16(&dev, SA_QDT + 4, tm_r16(&dev, p16_to_off(n2) + 2));

		mvb_md_dispatcher(&dev);
		check(dev.rcv_filled == 1 && dev.p16_rq == n1,
		      "erster Aufruf holte %d Frames", dev.rcv_filled);
		mvb_md_dispatcher(&dev);
		check(dev.rcv_filled == 2 && dev.p16_rq == n2,
		      "zweiter Aufruf: %d Frames", dev.rcv_filled);
		mvb_md_dispatcher(&dev);
		check(dev.rcv_filled == 2, "dritter Aufruf holte mehr");
		printf("  Zwei anstehende Frames: je Aufruf eines\n");
	}
}

/* ---------------------------------------------------------- Test 8 */
/* Adressierung der physischen Ports gegen die Werte aus dem Dekompilat. */
static void test_pp_offsets(void)
{
	printf("\n--- Test 8: Adressen der physischen Ports ---\n");

	check(tm_dock_offset(TM_PP_EF0, 0) == 0x40, "EF0 Seite0 falsch");
	check(tm_dock_offset(TM_PP_EF0, 1) == 0x60, "EF0 Seite1 falsch");
	check(tm_dock_offset(TM_PP_EF1, 0) == 0x48, "EF1 Seite0 falsch");
	check(tm_dock_offset(TM_PP_EF1, 1) == 0x68, "EF1 Seite1 falsch");
	check(tm_dock_offset(TM_PP_FC15, 0) == 0x58, "FC15 Seite0 falsch");
	check(tm_dock_offset(TM_PP_MSRC, 0) == 0x80, "MSRC Seite0 falsch");
	check(tm_dock_offset(TM_PP_MSNK, 1) == 0xE0, "MSNK Seite1 falsch");
	printf("  EF0 0x40/0x60  EF1 0x48/0x68  FC15 0x58  "
	       "MSRC 0x80  MSNK1 0xE0\n");

	/* Dock-Formel gegen die gemessene Zuordnung Port 491 -> Index 213 */
	check(tm_dock_offset(213, 0) == 3400,
	      "Dock 213 Seite0 ist %u statt 3400", tm_dock_offset(213, 0));
	printf("  Dock 213 Seite0 bei +3400, Seite1 bei +%u\n",
	       tm_dock_offset(213, 1));
}

/* ---------------------------------------------------------- Test 9 */
/*
 * mvb_hardw_config: das Antwortfenster muss im SCR landen, ohne die
 * uebrigen Bits anzufassen, und der Zweileitungsbetrieb muss SLM im DR
 * loeschen. Zusaetzlich muss der Leitungszustand im Device Status Word
 * ankommen. Sollwert ist der am Geraet gemessene Registerstand.
 */
static void test_hardw_config(void)
{
	const u32 pcs1 = SA_PP_PCS + TM_PP_FC15 * 8 + 2;
	u16 scr, dr, dsw;

	printf("\n--- Test 9: Antwortfenster und Leitungsbetrieb ---\n");

	dev.configured = 1;
	drvdata.dev[0] = dev;		/* mvb_set_laa_rld laeuft ueber drvdata */

	/* Ausgangslage: gemessener Stand mit TMO_21US und gesetztem SLM */
	sa_w16(&drvdata.dev[0], MVBC_SCR, 0x83c7);
	sa_w16(&drvdata.dev[0], MVBC_DR, 0x150d);
	sa_w16(&drvdata.dev[0], pcs1, 0);

	check(mvb_hardw_config(&drvdata.dev[0], MVB_LINE_BOTH, 1) == 0,
	      "mvb_hardw_config(BOTH, 1) schlug fehl");

	scr = sa_r16(&drvdata.dev[0], MVBC_SCR);
	dr  = sa_r16(&drvdata.dev[0], MVBC_DR);

	check(scr == 0x87c7, "SCR ist 0x%04x statt 0x87C7", scr);
	check((scr & TM_SCR_TMO_MASK) == TM_SCR_TMO_43US,
	      "TMO-Feld ist 0x%04x statt TMO_43US", scr & TM_SCR_TMO_MASK);
	check((dr & TM_DR_SLM) == 0, "SLM nicht geloescht, DR = 0x%04x", dr);
	check(dr == 0x150c, "DR ist 0x%04x statt 0x150C", dr);
	printf("  SCR 0x83C7 -> 0x%04x (TMO_43US), DR 0x150D -> 0x%04x\n",
	       scr, dr);

	/*
	 * DR 0x150C traegt LAA (Bit 3) und das gespeicherte RLD (Bit 12).
	 * auto_reset_rld = 0, also kommt RLD aus Bit 12 - beide Bits muessen
	 * im DSW stehen.
	 */
	dsw = sa_r16(&drvdata.dev[0],
		     SA_PP_DATA + tm_dock_offset(TM_PP_FC15,
			(sa_r16(&drvdata.dev[0], pcs1) & TM_PCS_VP_MSK) ? 1 : 0));
	check((dsw & (MVB_DSW_LAA | MVB_DSW_RLD)) ==
	      (MVB_DSW_LAA | MVB_DSW_RLD),
	      "DSW ist 0x%04x, LAA/RLD fehlen", dsw);
	printf("  Device Status Word 0x%04x (LAA + RLD gesetzt)\n", dsw);

	/* auto_reset_rld = 1 nimmt RLD aus DR Bit 2 - das ist hier 1 */
	drvdata.dev[0].auto_reset_rld = 1;
	mvb_set_laa_rld();
	dsw = sa_r16(&drvdata.dev[0],
		     SA_PP_DATA + tm_dock_offset(TM_PP_FC15,
			(sa_r16(&drvdata.dev[0], pcs1) & TM_PCS_VP_MSK) ? 1 : 0));
	check((dsw & MVB_DSW_RLD) == MVB_DSW_RLD,
	      "RLD aus DR Bit 2 fehlt, DSW = 0x%04x", dsw);

	/* Beide Seiten des doppelt gepufferten Ports muessen gleich sein */
	check(sa_r16(&drvdata.dev[0], SA_PP_DATA + tm_dock_offset(TM_PP_FC15, 0)) ==
	      sa_r16(&drvdata.dev[0], SA_PP_DATA + tm_dock_offset(TM_PP_FC15, 1)),
	      "die beiden DSW-Seiten tragen verschiedene Werte");
	printf("  beide Seiten des DSW-Ports gleich\n");

	/* treply_config > 3 wird abgelehnt, das SCR bleibt unberuehrt */
	scr = sa_r16(&drvdata.dev[0], MVBC_SCR);
	check(mvb_hardw_config(&drvdata.dev[0], MVB_LINE_BOTH, 4) != 0,
	      "treply_config = 4 wurde nicht abgelehnt");
	check(sa_r16(&drvdata.dev[0], MVBC_SCR) == scr,
	      "SCR wurde trotz Ablehnung veraendert");

	/* unbekannter Leitungsbetrieb wird ebenfalls abgelehnt */
	check(mvb_hardw_config(&drvdata.dev[0], 7, 1) != 0,
	      "line_config = 7 wurde nicht abgelehnt");

	drvdata.dev[0].configured = 0;
	dev.configured = 0;
}

/* --------------------------------------------------------- Test 10 */
/*
 * Die vier Interruptquellen muessen zusammen genau die am Geraet
 * gemessenen Masken ergeben. Dazu die Wartezyklen im SCR, die der
 * Vergleich gegen den Originaltreiber aufgedeckt hat.
 */
static void test_interrupts(void)
{
	printf("\n--- Test 10: Interruptmasken und Wartezyklen ---\n");

	dev.int_mask[0] = 0;
	dev.int_mask[1] = 0;
	sa_w16(&dev, MVBC_IMR0, 0);
	sa_w16(&dev, MVBC_IMR1, 0);

	mvb_int_connect(&dev, MVB_INT_FEV);
	mvb_int_connect(&dev, MVB_INT_DTI2);
	mvb_int_connect(&dev, MVB_INT_DTI1);
	mvb_int_connect(&dev, MVB_INT_RQE);

	check(sa_r16(&dev, MVBC_IMR0) == 0x0003,
	      "IMR0 ist 0x%04x statt 0x0003", sa_r16(&dev, MVBC_IMR0));
	check(sa_r16(&dev, MVBC_IMR1) == 0x0880,
	      "IMR1 ist 0x%04x statt 0x0880", sa_r16(&dev, MVBC_IMR1));
	check(dev.int_mask[0] == 0x0003 && dev.int_mask[1] == 0x0880,
	      "Maskenspiegel stimmt nicht mit den Registern ueberein");
	printf("  IMR0 0x%04x, IMR1 0x%04x (gemessen 0x0003 / 0x0880)\n",
	       sa_r16(&dev, MVBC_IMR0), sa_r16(&dev, MVBC_IMR1));

	/* Bitzuordnung einzeln, damit ein Zahlendreher auffaellt */
	check(MVB_INT_BIT(MVB_INT_DTI1) == 0x0001, "DTI1 != IMR0 Bit 0");
	check(MVB_INT_BIT(MVB_INT_DTI2) == 0x0002, "DTI2 != IMR0 Bit 1");
	check(MVB_INT_BIT(MVB_INT_FEV)  == 0x0080, "FEV != IMR1 Bit 7");
	check(MVB_INT_BIT(MVB_INT_RQE)  == 0x0800, "RQE != IMR1 Bit 11");

	/* Wartezyklen: das SCR-Feld muss 0x0300 tragen */
	check(TM_SCR_WS_3 == 0x0300, "TM_SCR_WS_3 ist nicht 0x0300");
	check((TM_SCR_WS_3 | 0x84c5) == 0x87c5,
	      "SCR aus Wartezyklen und Grundwert ergibt nicht 0x87C5");
	check((TM_SCR_WS_3 | 0x04c0) == 0x07c0,
	      "SCR an der Sondieradresse ergibt nicht 0x07C0");
	printf("  SCR aktiv 0x%04x, Sondieradresse 0x%04x\n",
	       TM_SCR_WS_3 | 0x84c5, TM_SCR_WS_3 | 0x04c0);

	/*
	 * Zaehleruebertrag: FEV traegt die Hardwarestaende in den
	 * Statusblock nach und leert die Hardware.
	 */
	memset(&dev.status, 0, sizeof(dev.status));
	dev.status.frames = 100;
	sa_w16(&dev, MVBC_FC,  7);
	sa_w16(&dev, MVBC_EC,  5);
	sa_w16(&dev, MVBC_ECA, 3);
	sa_w16(&dev, MVBC_ECB, 1);
	mvb_fev_handler(&dev);

	check(dev.status.frames == 107, "frames ist %u statt 107",
	      dev.status.frames);
	check(dev.status.errors == 5 && dev.status.errors_a == 3 &&
	      dev.status.errors_b == 1, "Fehlerzaehler falsch uebernommen");
	check(sa_r16(&dev, MVBC_FC) == 0 && sa_r16(&dev, MVBC_EC) == 0 &&
	      sa_r16(&dev, MVBC_ECA) == 0 && sa_r16(&dev, MVBC_ECB) == 0,
	      "Hardwarezaehler wurden nicht geleert");
	printf("  Uebertrag 100+7 = %u, Hardware geleert\n", dev.status.frames);

	/* Ueberlauf des Softwarezaehlers setzt alle vier zurueck */
	dev.status.frames = 0xffffffffu;
	sa_w16(&dev, MVBC_FC, 2);
	mvb_fev_handler(&dev);
	check(dev.status.frames == 0 && dev.status.errors == 0,
	      "Ueberlauf setzt die Zaehler nicht zurueck");
	printf("  Ueberlauf setzt alle vier Zaehler zurueck\n");

	memset(&dev.status, 0, sizeof(dev.status));
}

/* --------------------------------------------------------- Test 11 */
/*
 * Quittung ueber das Vektorregister. Das Register meldet mit Bit 8 eine
 * gueltige Nummer; jedes Lesen muss die naechste holen, bis nichts mehr
 * ansteht. Der Zaehler im Abspielwerk gibt eine feste Folge vor.
 */
static u16 ivr_queue[8];
static int ivr_pos, ivr_len;
static int fev_calls, dti2_calls;

static void test_ivr_drain(void)
{
	u32 ivr0 = (u32)((u8 *)dev.p_sa - fake_tm) + MVBC_IVR0;
	int i;

	printf("\n--- Test 11: Quittung ueber das Vektorregister ---\n");

	/* drei anstehende Quellen, dann leer */
	ivr_queue[0] = 0x100 | MVB_INT_DTI2;
	ivr_queue[1] = 0x100 | MVB_INT_DTI2;
	ivr_queue[2] = 0x100 | MVB_INT_DTI1;
	ivr_queue[3] = 0x0000;
	ivr_len = 4;
	ivr_pos = 0;
	dti2_calls = 0;

	/* Das Abspielwerk liefert die Folge ueber den Speicher nach */
	for (i = 0; i < ivr_len; i++) {
		fake_tm[ivr0]     = ivr_queue[i] & 0xff;
		fake_tm[ivr0 + 1] = ivr_queue[i] >> 8;
		if (!(ivr_queue[i] & 0x100))
			break;
		mvb_run_int_handler(&dev, ivr_queue[i] & 0xff);
		if ((ivr_queue[i] & 0xff) == MVB_INT_DTI2)
			dti2_calls++;
	}

	check(dti2_calls == 2, "DTI2 %d mal statt zweimal behandelt", dti2_calls);

	/* Der echte Ablauf: Register leert sich, danach steht null drin */
	fake_tm[ivr0] = 0;
	fake_tm[ivr0 + 1] = 0;
	mvb_drain_ivr(&dev, MVBC_IVR0, 0);
	check(sa_r16(&dev, MVBC_IVR0) == 0,
	      "IVR0 wurde nicht auf null gesetzt");

	/* Ein haengendes Register darf den Kernel nicht festhalten */
	fake_tm[ivr0] = MVB_INT_DTI2;
	fake_tm[ivr0 + 1] = 0x01;      /* Bit 8 bleibt stehen */
	dti2_calls = 0;
	fev_calls = 0;
	mvb_drain_ivr(&dev, MVBC_IVR0, 0);
	check(sa_r16(&dev, MVBC_IVR0) == 0,
	      "haengendes IVR0 wurde nicht genullt");
	printf("  Schranke greift, IVR0 danach 0x%04x\n",
	       sa_r16(&dev, MVBC_IVR0));

	/* Die Nummern aus IVR1 liegen um 16 versetzt */
	check(16 + (MVB_INT_FEV - 16) == MVB_INT_FEV, "Versatz IVR1 falsch");
	check(MVB_INT_FEV - 16 == 7, "FEV meldet sich in IVR1 nicht als 7");
	check(MVB_INT_RQE - 16 == 11, "RQE meldet sich in IVR1 nicht als 11");
	printf("  IVR1 meldet FEV als 7, RQE als 11 (+16 Versatz)\n");
}

/* ---------------------------------------------------------- Test 12 */
/*
 * ioctl-ABI gegen das Disassembly des Originals. Der Geraetetest hat
 * gezeigt, dass WRITE_DEV_ADDR im Original das Argument als Wert nimmt;
 * dieselbe Pruefung deckt DISABLE_PORT, WRITE_DSW, MD_GET_STATUS,
 * HWINIT und die Ereignisaufzeichnung ab.
 */
static long ioc(unsigned int cmd, unsigned long arg)
{
	struct file f = { .private_data = &dev };

	return pixy_mvblli_ioctl(&f, cmd, arg);
}

static u16 pcs_w0(u16 port)
{
	return tm_r16(&dev, lp_pcs_off(&dev, lp_port_index(&dev, port)));
}

static u16 port_of_type(unsigned int type, u16 after)
{
	u16 p;

	for (p = after + 1; p < TM_PORT_COUNT; p++) {
		u16 idx = lp_port_index(&dev, p);

		if (idx && idx <= 0x3ff &&
		    ((pcs_w0(p) >> TM_PCS_TYPE_OFF) & 3) == type)
			return p;
	}
	return 0;
}

static void test_ioctl_abi(void)
{
	u16 v16 = 0, src, snk1, snk2, snk3;
	u32 v32;
	u16 pcs_before;
	mvb_rec_event ev;
	PixyMvblliConfigLpTs ts;
	struct { u16 count; } empty = { 0 };
	unsigned long a;
	u16 sel, rst, st;
	int i, pit_nonzero;

	printf("\n--- Test 12: ioctl-ABI wie im Maschinencode ---\n");

	dev.enable = 1;
	dev.status.is_init = 1;
	dev.status.is_active = 0;
	dev.prt_indx_max = 0x3ff;
	if (!dev.all_tacks)
		dev.all_tacks = calloc(TM_PORT_COUNT, sizeof(u16));

	src  = port_of_type(2, 0);
	snk1 = port_of_type(1, 0);
	snk2 = port_of_type(1, snk1);
	snk3 = port_of_type(1, snk2);
	check(src && snk1 && snk2 && snk3, "keine Quelle/Senken im Abzug");
	printf("  Quelle %u, Senken %u %u %u\n", src, snk1, snk2, snk3);

	/* WRITE_DEV_ADDR: Wert, nicht Zeiger */
	check(ioc(IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR, 0xF0) == 0,
	      "WRITE_DEV_ADDR 0xF0 als Wert abgelehnt");
	check(sa_r16(&dev, MVBC_DAOR) == 0xF0 && dev.mvb_addr == 0xF0,
	      "Adresse 0xF0 nicht gesetzt");
	check(ioc(IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR, (unsigned long)&v16) ==
	      -EINVAL, "WRITE_DEV_ADDR mit Zeiger nicht EINVAL");
	check(ioc(IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR, 0) == -EINVAL,
	      "WRITE_DEV_ADDR 0 nicht EINVAL");
	dev.status.is_active = 1;
	check(ioc(IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR, 0x10) == -EFAULT,
	      "WRITE_DEV_ADDR im Betrieb nicht EFAULT");
	dev.status.is_active = 0;
	check(ioc(IOCTL_PIXY_MVBLLI_READ_DEV_ADDR, (unsigned long)&v16) == 0 &&
	      v16 == 0xF0, "READ_DEV_ADDR liefert 0x%04x", v16);
	printf("  WRITE_DEV_ADDR nimmt den Wert, Zeiger ergibt EINVAL\n");

	/* WRITE_DSW: Wert, oberes Wort Maske */
	check(ioc(IOCTL_PIXY_MVBLLI_WRITE_DSW, 0x00020002) == 0,
	      "WRITE_DSW abgelehnt");
	ioc(IOCTL_PIXY_MVBLLI_READ_DSW, (unsigned long)&v16);
	check(v16 & 0x0002, "DSW Bit 1 nicht gesetzt (0x%04x)", v16);
	ioc(IOCTL_PIXY_MVBLLI_WRITE_DSW, 0x00020000);
	ioc(IOCTL_PIXY_MVBLLI_READ_DSW, (unsigned long)&v16);
	check(!(v16 & 0x0002), "DSW Bit 1 nicht geloescht (0x%04x)", v16);
	printf("  WRITE_DSW nimmt den Wert, Maske im oberen Wort\n");

	/* DISABLE_PORT: nur Quellen, nur die Typbits */
	check(ioc(IOCTL_PIXY_MVBLLI_DISABLE_PORT, 0x1000) == -EINVAL,
	      "DISABLE_PORT 0x1000 nicht EINVAL");
	pcs_before = pcs_w0(snk1);
	check(ioc(IOCTL_PIXY_MVBLLI_DISABLE_PORT, snk1) == -EIO,
	      "DISABLE_PORT auf Senke nicht EIO");
	check(pcs_w0(snk1) == pcs_before, "Senke wurde veraendert");
	pcs_before = pcs_w0(src);
	check(ioc(IOCTL_PIXY_MVBLLI_DISABLE_PORT, src) == 0,
	      "DISABLE_PORT auf Quelle abgelehnt");
	check(pcs_w0(src) == (pcs_before & 0xf3ff) && lp_port_index(&dev, src),
	      "Quelle: PCS 0x%04x -> 0x%04x, PIT %u", pcs_before, pcs_w0(src),
	      lp_port_index(&dev, src));
	printf("  DISABLE_PORT: Senke EIO, Quelle PCS 0x%04x -> 0x%04x\n",
	       pcs_before, pcs_w0(src));

	/* MD_GET_STATUS: Pruefung des Inhalts, Auswahl aus dem Zeiger */
	v32 = 0x00000008;
	check(ioc(IOCTL_PIXY_MVBLLI_MD_GET_STATUS, (unsigned long)&v32) ==
	      -EINVAL, "MD_GET_STATUS mit Bit 3 nicht EINVAL");
	dev.rq_overflow = 4;
	sa_w16(&dev, SA_PP_PCS + TM_PP_MSRC * 8 + 4, 0xffff);
	sa_w16(&dev, SA_PP_PCS + TM_PP_MSNK * 8 + 4, 0);
	a = (unsigned long)&v32;
	sel = (u16)(a >> 16);
	rst = (u16)a;
	v32 = 0x00050000;
	check(ioc(IOCTL_PIXY_MVBLLI_MD_GET_STATUS, a) == 0,
	      "MD_GET_STATUS abgelehnt");
	st = (u16)v32;
	check((v32 >> 16) == 0x0005, "MD_GET_STATUS schreibt mehr als 2 Byte");
	check(st == (0x0006 & sel), "Status 0x%04x statt 0x%04x", st,
	      0x0006 & sel);
	check(dev.rq_overflow == ((rst & 4) ? 0 : 4),
	      "rq_overflow %u, reset aus Zeiger 0x%04x", dev.rq_overflow, rst);
	printf("  MD_GET_STATUS: 2 Byte, selector 0x%04x/reset 0x%04x aus der "
	       "Adresse\n", sel, rst);

	/* Rahmen: FLUSH, unbekannte Nummern */
	check(ioc(IOCTL_PIXY_MVBLLI_MD_FLUSH_QUEUE, 1) == -EINVAL,
	      "FLUSH mit Argument nicht EINVAL");
	check(ioc(_IO('L', 0), 0) == -EINVAL, "Nummer 0 nicht EINVAL");
	check(ioc(_IOW('L', 2, u32), 0xF0) == -EINVAL,
	      "falsche Groesse nicht EINVAL");
	check(ioc(IOCTL_PIXY_MVBLLI_MD_NSDB, 1) == -EINVAL,
	      "MD_NSDB nicht EINVAL");
	check(ioc(_IO('L', 23), 0) == -ENOTTY, "Nummer 23 nicht ENOTTY");
	printf("  FLUSH mit Argument, Nummer 0 und falsche Groesse: EINVAL\n");

	/* Ereignisaufzeichnung */
	ev.ts_port = snk1; ev.buf_len = 0;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == -EINVAL,
	      "REC_CONF buf_len 0 nicht EINVAL");
	ev.buf_len = MVB_REC_BUF_SIZE + 1;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == -EINVAL,
	      "REC_CONF buf_len 21 nicht EINVAL");
	ev.ts_port = 0; ev.buf_len = 5;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == -EINVAL,
	      "REC_CONF Port 0 nicht EINVAL");
	ev.ts_port = src;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == -EIO,
	      "REC_CONF auf Quelle nicht EIO");
	ev.ts_port = snk1;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == 0,
	      "REC_CONF Senke 1 abgelehnt");
	check((pcs_w0(snk1) & 0x00e0) == 0x00e0, "DTI-Feld nicht 7");
	check(sa_r16(&dev, MVBC_IMR0) & 0x0040, "DTI7 nicht freigegeben");
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == -EBUSY,
	      "doppelter REC_CONF nicht EBUSY");
	ev.ts_port = snk2;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == 0,
	      "REC_CONF Senke 2 abgelehnt");
	ev.ts_port = snk3;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_CONF, (unsigned long)&ev) == -ENOSPC,
	      "dritter REC_CONF nicht ENOSPC");
	ev.ts_port = snk1;
	check(ioc(IOCTL_PIXY_MVBLLI_REC_DEL, (unsigned long)&ev) == 0,
	      "REC_DEL Senke 1 abgelehnt");
	check(!(pcs_w0(snk1) & 0x00e0), "DTI-Feld nicht geloescht");
	check(sa_r16(&dev, MVBC_IMR0) & 0x0040, "DTI7 zu frueh abgeschaltet");
	check(ioc(IOCTL_PIXY_MVBLLI_REC_DEL, (unsigned long)&ev) == -EINVAL,
	      "REC_DEL unbekannt nicht EINVAL");
	ev.ts_port = snk2;
	ioc(IOCTL_PIXY_MVBLLI_REC_DEL, (unsigned long)&ev);
	check(!(sa_r16(&dev, MVBC_IMR0) & 0x0040), "DTI7 bleibt an");
	printf("  REC_CONF/REC_DEL: DTI 7 im PCS, IMR0 Bit 6, EBUSY, ENOSPC\n");

	/* HWINIT */
	memset(&ts, 0, sizeof(ts));
	ts.prt_addr_max = 0x123;
	ts.prt_indx_max = 0x456;
	check(ioc(IOCTL_PIXY_MVBLLI_HWINIT, (unsigned long)&ts) == -EIO,
	      "HWINIT mit ts_type 0 nicht EIO");
	check(dev.prt_indx_max == 0x3ff && dev.prt_addr_max == 0xfff,
	      "Grenzen 0x%x/0x%x statt aus mcm", dev.prt_addr_max,
	      dev.prt_indx_max);
	ts.ts_type = 1;
	check(ioc(IOCTL_PIXY_MVBLLI_HWINIT, (unsigned long)&ts) == 0,
	      "HWINIT ts_type 1 abgelehnt");
	check(lp_port_index(&dev, snk1), "ownership 0 hat die PIT geleert");
	ts.ownership = 1;
	check(ioc(IOCTL_PIXY_MVBLLI_HWINIT, (unsigned long)&ts) == 0,
	      "HWINIT ownership 1 abgelehnt");
	pit_nonzero = 0;
	for (i = 0; i < TM_PORT_COUNT; i++)
		pit_nonzero += lp_port_index(&dev, i) != 0;
	for (i = 0; i <= 0x3ff; i++)
		pit_nonzero += tm_r16(&dev, lp_pcs_off(&dev, i)) != 0;
	check(!pit_nonzero, "%d Eintraege in PIT/PCS stehen geblieben",
	      pit_nonzero);
	printf("  HWINIT: ts_type 0 EIO, Grenzen 0xfff/0x3ff aus mcm, "
	       "ownership 1 leert PIT und PCS\n");

	/* PD_CONF mit leerer Liste */
	check(ioc(IOCTL_PIXY_MVBLLI_PD_CONF, (unsigned long)&empty) == 0,
	      "PD_CONF leer abgelehnt");
	check(sa_r16(&dev, MVBC_STSR) == 0x1004, "STSR 0x%04x statt 0x1004",
	      sa_r16(&dev, MVBC_STSR));
	printf("  PD_CONF mit leerer Liste: STSR 0x%04x\n",
	       sa_r16(&dev, MVBC_STSR));

	/* Doppelter Port in einer Liste: EIO, danach PIT und PCS leer */
	{
		u16 dup[1 + 2 * 3] = { 2, 491, 4, 2, 491, 4, 2 };
		u16 two[1 + 2 * 3] = { 2, 181, 4, 1, 491, 4, 2 };

		check(ioc(IOCTL_PIXY_MVBLLI_PD_CONF, (unsigned long)dup) ==
		      -EIO, "doppelter Port nicht EIO");
		check(!lp_port_index(&dev, 491), "PIT nach Fehler nicht leer");
		check(ioc(IOCTL_PIXY_MVBLLI_PD_CONF, (unsigned long)two) == 0,
		      "PD_CONF 181/491 abgelehnt");
		check(pcs_w0(181) == 0x1400 && pcs_w0(491) == 0x1000,
		      "PCS 181 0x%04x, 491 0x%04x statt 0x1400/0x1000",
		      pcs_w0(181), pcs_w0(491));
		printf("  PD_CONF: Doppel EIO und geleert; Senke 0x%04x, "
		       "Quelle passiv 0x%04x\n", pcs_w0(181), pcs_w0(491));
	}
}

/* ---------------------------------------------------------- Test 13 */
/*
 * Ringe wie mvb_md_install_q: LLR k zeigt auf Puffer k, auch der
 * Waechter hat einen (Datenzeiger 0, Puffer 0 unbenutzt), alle Puffer
 * leer. Die Sollwerte fuer LLR 1 sind am Geraet unter dem Original
 * gemessen (mvbdiff p1-orig).
 */
static void test_ring_layout(void)
{
	static const struct { u32 off; u16 n; u16 llr1; } q[] = {
		{ MD_TQ0_OFFSET, MD_TQ0_LLRS, 0x2010 },
		{ MD_TQ1_OFFSET, MD_TQ1_LLRS, 0x2138 },
		{ MD_RQ_OFFSET,  MD_RQ_LLRS,  0x2910 },
	};
	unsigned int k, i;

	printf("\n--- Test 13: Ringbelegung wie im Original ---\n");

	for (k = 0; k < ARRAY_SIZE(q); k++) {
		u32 data = ALIGN(q[k].off + q[k].n * 4u, 32u);
		int bad_ptr = 0, bad_buf = 0;

		memset(fake_tm + q[k].off, 0xff, data + q[k].n * 32u - q[k].off);
		mvb_md_install_q(&dev, q[k].off, q[k].n);

		check(tm_r16(&dev, q[k].off) == 0, "Waechter nicht 0");
		check(tm_r16(&dev, q[k].off + 4) == q[k].llr1,
		      "LLR 1 zeigt auf 0x%04x statt 0x%04x",
		      tm_r16(&dev, q[k].off + 4), q[k].llr1);
		for (i = 1; i < q[k].n; i++)
			bad_ptr += tm_r16(&dev, q[k].off + i * 4) !=
				   (data + i * 32) / 4;
		for (i = 0; i < q[k].n * 16u; i++)
			bad_buf += tm_r16(&dev, data + i * 2) != 0;
		check(!bad_ptr, "%d Datenzeiger falsch", bad_ptr);
		check(!bad_buf, "%d Pufferworte nicht geleert", bad_buf);
		check(data + q[k].n * 32u <= (k < 2 ? q[k + 1].off : 0xFC00u),
		      "Puffer ueberlappen den naechsten Ring");
		printf("  Ring @0x%05x: %3u LLR, LLR 1 -> 0x%04x, Puffer bis "
		       "0x%05x\n", q[k].off, q[k].n, tm_r16(&dev, q[k].off + 4),
		       data + q[k].n * 32u);
	}
}

/* ---------------------------------------------------------- Test 14 */
/*
 * Schliessen wie im Original: mvb_stop loescht nur Bit 1 des IL-Felds,
 * mvb_deinit setzt SCR = 0 (RESET), das BCR verliert die
 * Interruptnummer. Am Geraet stand der Nachbau danach noch in CONFIG.
 */
static void test_deinit(void)
{
	/* ISA-Fenster im ungenutzten da_data-Bereich der Attrappe */
	u8 *isa = fake_tm + 0x3F000;

	printf("\n--- Test 14: Controller nach close() ---\n");

	sa_w16(&dev, MVBC_SCR, 0x87C7);
	mvb_stop(&dev);
	check(sa_r16(&dev, MVBC_SCR) == 0x87C5, "mvb_stop: SCR 0x%04x",
	      sa_r16(&dev, MVBC_SCR));

	dev.pisa = isa;
	iowrite16(0x2611, isa + ISA_BCR);
	sa_w16(&dev, MVBC_SCR, 0x87C7);
	sa_w16(&dev, MVBC_IMR0, 0x0043);
	dev.status.is_init = 1;
	dev.irq_attached = 0;
	mvb_deinit_board(&dev);
	check(sa_r16(&dev, MVBC_SCR) == 0, "SCR nach close 0x%04x statt 0",
	      sa_r16(&dev, MVBC_SCR));
	check(ioread16(isa + ISA_BCR) == 0x2600, "BCR 0x%04x statt 0x2600",
	      ioread16(isa + ISA_BCR));
	check(sa_r16(&dev, MVBC_IMR0) == 0, "IMR0 nicht geleert");
	check(!dev.status.is_init, "Status nicht geleert");
	printf("  mvb_stop 0x87C7 -> 0x87C5; close: SCR 0x%04x, BCR 0x%04x\n",
	       sa_r16(&dev, MVBC_SCR), ioread16(isa + ISA_BCR));
}

/* ---------------------------------------------------------- Test 15 */
/*
 * MD_FLUSH_QUEUE wie lm_m_v_send_queue_flush: MR = 0x0800, beide
 * Sendeeintraege der QDT auf 0, Ringe und Softwareposition unberuehrt.
 * Das naechste Senden traegt die QDT ab der aktuellen Position neu ein.
 * Dazu MSNK Bit 5 nur im Interruptbetrieb.
 */
static void test_flush_and_irq_mode(void)
{
	static u8 before[0xC000 - MD_TQ0_OFFSET];
	u16 pkt[16] = { 0 };
	u16 pos;
	int saved = mvb_irq;

	printf("\n--- Test 15: Sendequeue verwerfen, Interruptbetrieb ---\n");

	dev.q_tq_priority = 0;
	mvb_irq = 0;
	mvb_md_q_init(&dev);
	check(sa_r16(&dev, SA_PP_PCS + TM_PP_MSNK * 8) == 0xc404,
	      "MSNK bei irq=0: 0x%04x", sa_r16(&dev, SA_PP_PCS + TM_PP_MSNK * 8));
	mvb_irq = 7;
	mvb_md_q_init(&dev);
	check(sa_r16(&dev, SA_PP_PCS + TM_PP_MSNK * 8) == 0xc424,
	      "MSNK bei irq=7: 0x%04x", sa_r16(&dev, SA_PP_PCS + TM_PP_MSNK * 8));
	mvb_irq = saved;

	check(mvb_sndp(&dev, 6, 0, pkt) == 0, "Senden abgelehnt");
	check(mvb_sndp(&dev, 6, 0, pkt) == 0, "zweites Senden abgelehnt");
	pos = dev.p16_tq1;
	memcpy(before, fake_tm + MD_TQ0_OFFSET, sizeof(before));

	mvb_md_flush_send_queue(&dev);
	check(sa_r16(&dev, MVBC_MR) == 0x0800, "MR 0x%04x statt 0x0800",
	      sa_r16(&dev, MVBC_MR));
	check(!sa_r16(&dev, SA_QDT) && !sa_r16(&dev, SA_QDT + 2),
	      "QDT-Sendeeintraege nicht 0");
	check(!memcmp(before, fake_tm + MD_TQ0_OFFSET, sizeof(before)) &&
	      dev.p16_tq1 == pos, "Ringe oder Position veraendert");

	check(mvb_sndp(&dev, 6, 0, pkt) == 0, "Senden nach Flush abgelehnt");
	check(sa_r16(&dev, SA_QDT + 2) == pos,
	      "QDT xmit_q1 0x%04x statt Position 0x%04x",
	      sa_r16(&dev, SA_QDT + 2), pos);
	printf("  Flush: MR 0x0800, Ringe unveraendert, danach QDT ab 0x%04x\n",
	       pos);
	printf("  MSNK: irq=0 -> 0xC404, irq=7 -> 0xC424\n");

	/* poll(): eine Meldung je DTI1, ohne Interruptbetrieb nie */
	{
		struct file f = { .private_data = &dev };
		unsigned int r1, r2, r3, i, hits = 0;

		dev.md_rcv_irq_dispatched = dev.md_rcv_irq_signaled = 0;
		mvb_irq = 0;
		mvb_run_int_handler(&dev, MVB_INT_DTI1);
		check(pixy_mvblli_poll(&f, NULL) == 0, "poll bei irq=0 nicht 0");
		check(dev.md_rcv_irq_dispatched == 0,
		      "DTI1 bei irq=0 gezaehlt");
		mvb_irq = 7;
		check(pixy_mvblli_poll(&f, NULL) == 0, "poll ohne Meldung nicht 0");
		mvb_run_int_handler(&dev, MVB_INT_DTI1);
		mvb_run_int_handler(&dev, MVB_INT_DTI1);
		r1 = pixy_mvblli_poll(&f, NULL);
		r2 = pixy_mvblli_poll(&f, NULL);
		r3 = pixy_mvblli_poll(&f, NULL);
		check(r1 == 0x41 && r2 == 0x41 && r3 == 0,
		      "poll nach zwei Meldungen 0x%x 0x%x 0x%x", r1, r2, r3);
		for (i = 0; i < 20; i++) {
			mvb_run_int_handler(&dev, MVB_INT_DTI1);
			hits += pixy_mvblli_poll(&f, NULL) == 0x41;
		}
		check(hits == 20, "Zaehler modulo 16: %u von 20", hits);
		mvb_irq = saved;
		printf("  poll: irq=0 -> 0; irq=7 -> je DTI1 einmal POLLIN|RDNORM\n");
	}
}

/* ---------------------------------------------------------- Test 16 */
/*
 * Quittung bis zum leeren Durchgang. Nachgestellt wird das Rennen, das
 * am Geraet den Interrupt stillgelegt hat: Waehrend IVR0 geleert wird,
 * meldet IVR1 eine neue Quelle (FEV). Das Original kehrt dann mit
 * gesetzter Leitung zurueck; mit irq_rearm muss der zweite Durchgang
 * die Quelle abholen und ein dritter beide Register leer sehen.
 */
static u16 seq_ivr1[8], seq_ivr0[8];
static int seq_n1, seq_n0, pos_ivr1, pos_ivr0;
static unsigned long off_ivr1, off_ivr0;

static int ivr_script(unsigned long off, u16 *val)
{
	if (off == off_ivr1) {
		*val = pos_ivr1 < seq_n1 ? seq_ivr1[pos_ivr1] : 0;
		pos_ivr1++;
		return 1;
	}
	if (off == off_ivr0) {
		*val = pos_ivr0 < seq_n0 ? seq_ivr0[pos_ivr0] : 0;
		pos_ivr0++;
		return 1;
	}
	return 0;
}

static void run_race(int rearm, int *fev, int *dti2, int *rearms)
{
	int fev0 = dbg_fev, dti20 = dbg_dti2, re0 = dbg_rearm;
	int saved = irq_rearm;

	/* IVR1: FEV, leer | (waehrend IVR0) FEV, leer | leer */
	seq_ivr1[0] = 0x100 | (MVB_INT_FEV - 16);
	seq_ivr1[1] = 0;
	seq_ivr1[2] = 0x100 | (MVB_INT_FEV - 16);
	seq_ivr1[3] = 0;
	seq_n1 = 4;
	/* IVR0: DTI2, leer | leer | leer */
	seq_ivr0[0] = 0x100 | MVB_INT_DTI2;
	seq_ivr0[1] = 0;
	seq_n0 = 2;
	pos_ivr1 = pos_ivr0 = 0;

	irq_rearm = rearm;
	fake_ioread_hook = ivr_script;
	mvblli_irq_server(&dev);
	fake_ioread_hook = NULL;
	irq_rearm = saved;

	*fev = dbg_fev - fev0;
	*dti2 = dbg_dti2 - dti20;
	*rearms = dbg_rearm - re0;
}

static void test_irq_rearm(void)
{
	int fev, dti2, rearms;
	int enable = dev.enable, init = dev.status.is_init;

	printf("\n--- Test 16: Interruptquittung bis zum leeren Durchgang ---\n");

	off_ivr1 = (unsigned long)((u8 *)dev.p_sa - fake_tm) + MVBC_IVR1;
	off_ivr0 = (unsigned long)((u8 *)dev.p_sa - fake_tm) + MVBC_IVR0;
	dev.enable = 1;
	dev.status.is_init = 1;

	run_race(0, &fev, &dti2, &rearms);
	check(fev == 1 && dti2 == 1, "irq_rearm=0: FEV %d, DTI2 %d", fev, dti2);
	check(pos_ivr1 == 2, "irq_rearm=0: IVR1 %d mal gelesen statt 2", pos_ivr1);
	printf("  irq_rearm=0 (Original): FEV %d, DTI2 %d - zweite FEV bleibt "
	       "liegen\n", fev, dti2);

	run_race(1, &fev, &dti2, &rearms);
	check(fev == 2 && dti2 == 1, "irq_rearm=1: FEV %d, DTI2 %d", fev, dti2);
	check(rearms == 1, "irq_rearm=1: %d Nachdurchgaenge statt 1", rearms);
	check(pos_ivr1 == 5 && pos_ivr0 == 4,
	      "irq_rearm=1: IVR1 %d, IVR0 %d mal gelesen", pos_ivr1, pos_ivr0);
	check(sa_r16(&dev, MVBC_IVR0) == 0 && sa_r16(&dev, MVBC_IVR1) == 0,
	      "IVR nach dem Leeren nicht genullt");
	printf("  irq_rearm=1: FEV %d, DTI2 %d, %d Nachdurchgang, letzter "
	       "Durchgang leer\n", fev, dti2, rearms);

	/* Ruhiger Fall: nichts gemeldet, genau ein Durchgang */
	seq_n1 = seq_n0 = 0;
	pos_ivr1 = pos_ivr0 = 0;
	fake_ioread_hook = ivr_script;
	mvblli_irq_server(&dev);
	fake_ioread_hook = NULL;
	check(pos_ivr1 == 1 && pos_ivr0 == 1,
	      "leerer Interrupt: IVR1 %d, IVR0 %d mal gelesen", pos_ivr1, pos_ivr0);

	dev.enable = enable;
	dev.status.is_init = init;
}

int main(int argc, char **argv)
{
	const char *dir = (argc > 1) ? argv[1] : "mvbsnap";
	char path[512];

	printf("=== Abspielen des Treibercodes gegen den echten "
	       "Speicherabzug ===\n");

	snprintf(path, sizeof(path), "%s/tm_low.bin", dir);
	load(path, 0);
	snprintf(path, sizeof(path), "%s/tm_high.bin", dir);
	load(path, 0x10000);

	setup_dev();

	test_pp_offsets();
	test_read_all_ports();
	test_rings();
	test_dispatcher_empty();
	test_allocator();
	test_pd_roundtrip();
	test_md_send();
	test_md_receive();
	test_hardw_config();
	test_interrupts();
	test_ivr_drain();
	test_ioctl_abi();
	test_ring_layout();
	test_flush_and_irq_mode();
	test_irq_rearm();
	test_deinit();

	printf("\n=== %d Pruefungen, %d Fehler ===\n", checks, fails);
	return fails ? 1 : 0;
}
