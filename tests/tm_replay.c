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
unsigned long fake_io_writes;

#include "../recipes-kernel/pixy-mvblli/files/pixy-mvblli.c"

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

	/* PCS-Woerter gegen das Original */
	{
		int diff = 0;

		for (i = 0; i < count; i++) {
			u16 w0 = tm_r16(&dev, lp_pcs_off(&dev, new_idx[i]));
			u16 exp = ((u16)lp_len_2_fcode(list[i].size)
				   << TM_PCS_FCODE_OFF) |
				  (list[i].type << TM_PCS_TYPE_OFF);

			if (w0 != exp)
				diff++;
		}
		check(diff == 0, "%d PCS-Woerter weichen ab", diff);
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

			check(rc == 8, "Schreiben auf Senke %d liefert %d "
			      "statt 8", p, rc);
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

	printf("\n=== %d Pruefungen, %d Fehler ===\n", checks, fails);
	return fails ? 1 : 0;
}
