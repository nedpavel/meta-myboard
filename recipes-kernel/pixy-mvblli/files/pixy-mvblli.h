/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pixy-mvblli.h - ABI des MVB-Link-Layer-Treibers
 *
 * Schnittstelle zwischen /dev/mvblliN und dem Userspace. Die Werte sind
 * ABI: libpixymvb/libmvbrtp sind dagegen uebersetzt und duerfen nicht
 * neu gebaut werden muessen.
 */

#ifndef PIXY_MVBLLI_H
#define PIXY_MVBLLI_H

#include <asm/ioctl.h>

#ifndef __KERNEL__
#include <stdint.h>
#else
#include <linux/types.h>
#endif

/* Blockkennungen der Node-Supervisor-Datenbank (NSDB) */
#define NSDB_HEADER		0x0001
#define NS_RTP_ANNOUNCE_DEV	0x0010
#define NS_RTP_INS_DIR_ENTR	0x0020
#define NS_TRAFFIC_STORE	0x0080
#define NS_SINKTIME_SUPERVISION	0x0090
#define NS_MVBC_INIT		0x0200
#define NS_MVB_BA		0x0210
#define NS_ROM_FCTS_BASE	0x1300
#define NS_ROM_NAME_BASE	0x1310
#define NS_ROM_STATION_BASE	0x1320
#define END_OF_NSDB		0x000f

#define MVB_MAX_PORT_SIZE	32
#define MVB_MAX_PORT_SIZE_WORD	(MVB_MAX_PORT_SIZE / sizeof(uint16_t))

/* Werte fuer mvb_port.type */
#define IOCTL_PIXY_MVBLLI_PD		1
#define IOCTL_PIXY_MVBLLI_MD_HIGH	2
#define IOCTL_PIXY_MVBLLI_MD_LOW	3

typedef struct mvb_stat {
	uint16_t is_active;
	uint16_t is_init;
	uint16_t has_pd;
	uint16_t has_md;
	uint16_t has_ba;
	uint16_t mvb_addr;
	uint16_t extra_status;
	uint32_t frames;
	uint32_t errors;
	uint32_t errors_a;
	uint32_t errors_b;
	char hw_version[32];
	char sw_version[32];
	uint16_t t_ignore;
	uint16_t line_config;
} mvb_stat;

typedef struct mvb_ctrl {
	uint16_t dev_addr;
	uint16_t t_ignore;
	struct {
		uint8_t aon :1;
		uint8_t aof :1;
		uint8_t spl :1;
		uint8_t tms :1;
		uint8_t sla :1;
		uint8_t slb :1;
		uint8_t cla :1;
		uint8_t clb :1;
	} __attribute__((__packed__))
	  __attribute((scalar_storage_order("big-endian"))) command;
} mvb_ctrl;

/*
 * Der Datencontainer von read() und write(). Achtung: bei write() mit
 * type == MD_* ueberschreibt der Treiber die ersten vier Byte von data[]
 * mit dem Link_Header; bei read() enthaelt data[] den kompletten Frame
 * einschliesslich Link_Header.
 */
typedef struct mvb_port {
	uint16_t type;
	uint16_t port;
	uint16_t data[MVB_MAX_PORT_SIZE_WORD];
	uint16_t freshness;
} mvb_port;

typedef struct mvb_rec_event {
	uint16_t ts_port;
	uint16_t buf_len;
} mvb_rec_event;

typedef struct mvb_config_nsdb {
	uint32_t length;
	uint16_t *nsdb;
} mvb_config_nsdb;

typedef struct mvb_tm {
	int ts_id;
	uint16_t *address;
	int size_id;
} mvb_tm;

typedef struct PixyMvblliConfigLpPrt {
	uint16_t prt_addr;	/* MVB-Portadresse 0..4095 */
	uint16_t size;		/* Portgroesse in Byte: 2,4,8,16,32 */
	uint16_t type;		/* 0 = passiv, 1 = Senke, 2 = Quelle */
} PixyMvblliConfigLpPrt;

typedef struct PixyMvblliConfigLpTs {
	uint16_t *pb_mwd;
	uint8_t ownership;
	uint8_t ts_type;
	uint16_t prt_addr_max;
	uint16_t prt_indx_max;
	uint8_t auto_reset_rld;
} PixyMvblliConfigLpTs;

typedef struct PixyMvblliConfigPorts {
	uint16_t prt_count;
	PixyMvblliConfigLpPrt prt_list[];
} __attribute__((__packed__)) PixyMvblliConfigPorts;

typedef struct PixyMvblliConfigMex {
	uint16_t q_tq_priority;
} __attribute__((__packed__)) PixyMvblliConfigMex;

#define PIXY_PIXY_MVBLLI_IOCTL_MAGIC	'L'

#define IOCTL_PIXY_MVBLLI_READ_DEV_ADDR		_IOR(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  1, uint16_t)
#define IOCTL_PIXY_MVBLLI_WRITE_DEV_ADDR	_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  2, uint16_t)
#define IOCTL_PIXY_MVBLLI_READ_DSW		_IOR(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  3, uint16_t)
#define IOCTL_PIXY_MVBLLI_WRITE_DSW		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  4, uint32_t)
#define IOCTL_PIXY_MVBLLI_START			_IO (PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  5)
#define IOCTL_PIXY_MVBLLI_STOP			_IO (PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  6)
#define IOCTL_PIXY_MVBLLI_RETRIGGER		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  7, uint32_t)
#define IOCTL_PIXY_MVBLLI_READ_STATS		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  8, struct mvb_stat)
#define IOCTL_PIXY_MVBLLI_REC_CONF		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC,  9, struct mvb_rec_event)
#define IOCTL_PIXY_MVBLLI_REC_DEL		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 10, struct mvb_rec_event)
#define IOCTL_PIXY_MVBLLI_PD_NSDB		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 11, mvb_config_nsdb)
#define IOCTL_PIXY_MVBLLI_PD_CONF		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 12, PixyMvblliConfigPorts)
#define IOCTL_PIXY_MVBLLI_MD_NSDB		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 13, mvb_config_nsdb)
#define IOCTL_PIXY_MVBLLI_MD_CONF		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 14, PixyMvblliConfigMex)
#define IOCTL_PIXY_MVBLLI_BA_NSDB		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 15, mvb_config_nsdb)
#define IOCTL_PIXY_MVBLLI_DISABLE_PORT		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 16, uint16_t)
#define IOCTL_PIXY_MVBLLI_READ_TM		_IOR(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 17, struct mvb_tm)
#define IOCTL_PIXY_MVBLLI_MD_FLUSH_QUEUE	_IO (PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 18)
#define IOCTL_PIXY_MVBLLI_MD_GET_STATUS		_IOR(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 19, uint32_t)
#define IOCTL_PIXY_MVBLLI_WRITE_CONTROL		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 20, struct mvb_ctrl)
#define IOCTL_PIXY_MVBLLI_HWINIT		_IOW(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 21, PixyMvblliConfigLpTs)
#define IOCTL_PIXY_MVBLLI_USERS			_IOR(PIXY_PIXY_MVBLLI_IOCTL_MAGIC, 22, uint32_t)

#define PIXY_PIXY_MVBLLI_MAX_IOCTL_NR	22

#endif /* PIXY_MVBLLI_H */
