/*===========================================================================*/
/* NAME  : mvbc.h                                                            */
/* DATE  : 16.03.2023 Version 2.0                                            */
/* Author: Manuel Avancini                                                   */
/*===========================================================================*/




/*****************************************************************************/
/* Memo field                                                                */ 
/*****************************************************************************/
/*

 * The original Src of this file dates back to 1991 the time the MVBC01 ASIC
 * has been released. Minor adaptions have been made in order to support the
 * MVBIP. The MVBIP is an FPGA implementation of the MVB functionality based
 * on the MVBC02A ASIC excluding some of the bugs known.
 *
 * 28.05.2009 -mm-
 * Changes made for the decoder register in order to enable different PHY mode
 * operations for EMD and for OGF
 *
 * 25.03.2010 -mm-
 * Changes made for the decoder register in order to support different inter-
 * frame spacing times. This is required for mode EMD.
 *
 * 09.04.2010 -mm-
 * Traffic Memory Layout Structures defined.
*/				



										   																		   																			  
/******************************************************************************
/* Includes
/*****************************************************************************/


/******************************************************************************																			  
 * Compile Check
 *****************************************************************************/
#ifndef MVBC_H

    #define MVBC_H MVBC_H

/*  #endif  at bottom of file */

/*#define BIGENDIAN BIGENDIAN */

/******************************************************************************
 * Volatile attribute: Assures that register accesses are not optimized by "C"
 *****************************************************************************/
#ifndef VOL
    #define VOL volatile
#endif /* VOL */

/******************************************************************************
 * Data Structure: Bytes and Words
 *****************************************************************************/
#ifdef __WIN32__ /* Simulation Env. on Windows only */
    typedef unsigned char  TM_TYPE_BYTE;
    typedef unsigned short TM_TYPE_WORD;

    typedef VOL unsigned short TM_TYPE_RWORD; /* Always accessed directly */
#else
    typedef unsigned char  TM_TYPE_BYTE;
    typedef unsigned short TM_TYPE_WORD;

    typedef VOL unsigned short TM_TYPE_RWORD; /* Always accessed directly */
#endif /* __WIN32__ */

/******************************************************************************
 * Data Structure: Data Areas and Force Tables
 *****************************************************************************/
typedef union {
    TM_TYPE_BYTE   b[8];
    TM_TYPE_WORD   w[4];
} TM_TYPE_DOCK;

typedef union {
    TM_TYPE_BYTE   b[32];
    TM_TYPE_WORD   w[16];
    TM_TYPE_DOCK   dock[4];
} TM_TYPE_PAGE;  /* for 4 docks */

typedef union {
    TM_TYPE_PAGE   page[2];
    TM_TYPE_DOCK   pgdc[2][4];
    TM_TYPE_WORD   pgwd[2][16];
} TM_TYPE_DATA;  /* for 4 docks */


#define TM_PAGE_0       0      /* Data Area: Page 0                          */
#define TM_PAGE_1       1      /* Data Area: Page 1                          */
#define TM_FRC_DATA     0      /* Force Table Data Page                      */
#define TM_FRC_MASK     1      /* Force Table Mask Page                      */

#define MVBIP MVBIP         /* Define FPGA TYPE of MVB 28.05.09 -mm-         */

/******************************************************************************
 * Data Structure: Port Control and Status Register
 *****************************************************************************/
#pragma pack(2)
typedef union { /* PCS0 */
    struct {
#ifdef BIGENDIAN
        unsigned int f_code: 4;
        unsigned int src   : 1;
        unsigned int sink  : 1;
        unsigned int twcs  : 1;
        unsigned int wa    : 1;
        unsigned int dti   : 3;
        unsigned int cpe1  : 1;
        unsigned int cpe0  : 1;
        unsigned int qa    : 1;
        unsigned int num   : 1;
        unsigned int fe    : 1;
#else
        unsigned int fe    : 1;
        unsigned int num   : 1;
        unsigned int qa    : 1;
        unsigned int cpe0  : 1;
        unsigned int cpe1  : 1;
        unsigned int dti   : 3;
        unsigned int wa    : 1;
        unsigned int twcs  : 1;
        unsigned int sink  : 1;
        unsigned int src   : 1;
        unsigned int f_code: 4;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_PCS_WORD0;

#pragma pack(2)
typedef VOL union { /* PCS1 */
    struct {
#ifdef BIGENDIAN
        unsigned int dec   : 8;
        unsigned int ptd   : 1;
        unsigned int vp    : 1;
        unsigned int crc   : 1;
        unsigned int sqe   : 1;
        unsigned int alo   : 1;
        unsigned int bni   : 1;
        unsigned int terr  : 1;
        unsigned int sto   : 1;
#else
        unsigned int sto   : 1;
        unsigned int terr  : 1;
        unsigned int bni   : 1;
        unsigned int alo   : 1;
        unsigned int sqe   : 1;
        unsigned int crc   : 1;
        unsigned int vp    : 1;
        unsigned int ptd   : 1;
        unsigned int dec   : 8;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_PCS_WORD1;

#pragma pack(2)
typedef VOL union { /* PCS3 */
    struct {
#ifdef BIGENDIAN
        TM_TYPE_BYTE    crc1;
        TM_TYPE_BYTE    crc0;
#else
        TM_TYPE_BYTE    crc0;
        TM_TYPE_BYTE    crc1;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_PCS_CRCS;

#pragma pack(2)
typedef VOL union { /* PCS Structure per Port 4 Words */
    struct {
        TM_TYPE_PCS_WORD0 word0;
        TM_TYPE_PCS_WORD1 word1;
        TM_TYPE_RWORD     tack;
        TM_TYPE_PCS_CRCS  crcs;
    } reg;
    TM_TYPE_WORD w[4];
} TM_TYPE_PCS;


/* WORD 0 */
#define TM_PCS_FCODE_MSK    0xF000
#define TM_PCS_FCODE_OFF    12

#define TM_PCS_TYPE_MSK     0x0C00
#define TM_PCS_TYPE_OFF     10
#define TM_PCS_TYPE_CLR     0       /* !!! NEW */
#define TM_PCS_TYPE_SNK     1       /* !!! NEW */
#define TM_PCS_TYPE_SRC     2       /* !!! NEW */

#define TM_PCS_DTI_MSK      0x00E0
#define TM_PCS_DTI_OFF      5
#define TM_PCS_DTI_6        6       /* !!! NEW  BA SRCE */

#define TM_PCS_FE_MSK       0x0001
#define TM_PCS_FE_OFF       0
#define TM_PCS_FE_CLR       0
#define TM_PCS_FE_SET       1

#define TM_PCS_NUM_MSK      0x0002
#define TM_PCS_NUM_OFF      1
#define TM_PCS_NUM_CLR      0
#define TM_PCS_NUM_SET      1

/* WORD 1 */
#define TM_PCS_VP_MSK       0x0040
#define TM_PCS_VP_OFF       6
#define TM_PCS_DIAG_MSK     0x0007  /* !!! new !!! */
#define TM_PCS_DIAG_OFF     0       /* !!! new !!! */
#define TM_PCS_DIAG_CLR     0       /* !!! new !!! */
#define TM_PCS_DIAG_STO     1       /* !!! new !!! */
#define TM_PCS_DIAG_TERR    2       /* !!! new !!! */
#define TM_PCS_DIAG_RDY     3       /* !!! new !!! */
#define TM_PCS_DIAG_BNI     4       /* !!! new !!! */
/* MR VOL */
#define TM_MR_BUSY_MSK      0x0200
#define TM_MR_BUSY_OFF      9
#define TM_MR_BUSY_CLR      0
#define TM_MR_BUSY_SET      1

/* MFS VOL */
#define TM_MFS_PRT_ADDR_MSK     0x0FFF
#define TM_MFS_PRT_ADDR_OFF     0


/* !!! End of "New constants in update 1.1" */

/* Alternative bit specification for PCS */

/* F-Codes: use W_FC0, W_FC1, etc */

#define TM_PCS0_SRC     0x0800
#define TM_PCS0_SINK    0x0400
#define TM_PCS0_TWCS    0x0200
#define TM_PCS0_WA      0x0100
#define TM_PCS0_DTI7    0x00E0
#define TM_PCS0_DTI6    0x00C0
#define TM_PCS0_DTI5    0x00A0
#define TM_PCS0_DTI4    0x0080
#define TM_PCS0_DTI3    0x0060
#define TM_PCS0_DTI2    0x0040
#define TM_PCS0_DTI1    0x0020
#define TM_PCS0_CPE1    0x0010
#define TM_PCS0_CPE0    0x0008
#define TM_PCS0_QA      0x0004
#define TM_PCS0_NUM     0x0002
#define TM_PCS0_FE      0x0001

#define TM_PCS1_DEC     0xFF00
#define TM_PCS1_PTD     0x0080
#define TM_PCS1_VP1     0x0040
#define TM_PCS1_VP0     0x0000
#define TM_PCS1_CRC     0x0020
#define TM_PCS1_SQE     0x0010
#define TM_PCS1_ALO     0x0008
#define TM_PCS1_BNI     0x0004
#define TM_PCS1_TERR    0x0002
#define TM_PCS1_STO     0x0001

/******************************************************************************
 * MVBC Message Queue Structure
 *****************************************************************************/
#pragma pack(2)
typedef VOL struct {
    TM_TYPE_WORD   p16_data;
    TM_TYPE_WORD   p16_next;
} TM_TYPE_LLR;

/******************************************************************************
 * Data Structure: Queue Descriptor Table
 *****************************************************************************/
#pragma pack(2)
typedef VOL struct {
    TM_TYPE_WORD   xmit_q[2];
    TM_TYPE_WORD   rcve_q;
} TM_TYPE_QDT;

/******************************************************************************
 ******************************************************************************
 * Data Structure: Bitmaps of Internal Registers (MVBC)
 ******************************************************************************
 *****************************************************************************/

/******************************************************************************
 * Status Control Register: SCR
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int im    : 1;
        unsigned int quiet : 1;
        unsigned int mbc   : 1;
        unsigned int dmy__ : 1;  /* !!! Unsupported bit */
        unsigned int tmo   : 2;
        unsigned int ws    : 2;
        unsigned int arb   : 2;
        unsigned int uts   : 1;
        unsigned int utq   : 1;
        unsigned int mas   : 1;
        unsigned int rcev  : 1;
        unsigned int il    : 2;
#else
        unsigned int il    : 2;
        unsigned int rcev  : 1;
        unsigned int mas   : 1;
        unsigned int utq   : 1;
        unsigned int uts   : 1;
        unsigned int arb   : 2;
        unsigned int ws    : 2;
        unsigned int tmo   : 2;
        unsigned int dmy__ : 1;  /* !!! Unsupported bit */
        unsigned int mbc   : 1;
        unsigned int quiet : 1;
        unsigned int im    : 1;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_SCR;

#define TM_SCRX_WS_0        0
#define TM_SCRX_WS_1        1
#define TM_SCRX_WS_2        2
#define TM_SCRX_WS_3        3

#define TM_SCRX_ARB_0       0
#define TM_SCRX_ARB_1       1
#define TM_SCRX_ARB_2       2
#define TM_SCRX_ARB_3       3

#define TM_SCRX_TMO_21US    0
#define TM_SCRX_TMO_43US    1
#define TM_SCRX_TMO_64US    2
#define TM_SCRX_TMO_83US    3

/* Alternative: 16-bit Mask Values */

#define TM_SCR_IM           0x8000
#define TM_SCR_QUIET        0x4000
#define TM_SCR_MBC          0x2000

#define TM_SCR_TMO_MASK     0x0C00
#define TM_SCR_TMO_83US     0x0C00
#define TM_SCR_TMO_64US     0x0800
#define TM_SCR_TMO_43US     0x0400
#define TM_SCR_TMO_21US     0x0000

#define TM_SCR_WS_MASK      0x0300
#define TM_SCR_WS_3         0x0300
#define TM_SCR_WS_2         0x0200
#define TM_SCR_WS_1         0x0100
#define TM_SCR_WS_0         0x0000

#define TM_SCR_ARB_MASK     0x00C0
#define TM_SCR_ARB_3        0x00C0
#define TM_SCR_ARB_2        0x0080
#define TM_SCR_ARB_1        0x0040
#define TM_SCR_ARB_0        0x0000

#define TM_SCR_UTS          0x0020
#define TM_SCR_UTQ          0x0010
#define TM_SCR_MAS          0x0008
#define TM_SCR_RCEV         0x0004

#define TM_SCR_IL_MASK      0x0003
#define TM_SCR_IL_RUNNING   0x0003
#define TM_SCR_IL_TEST      0x0002
#define TM_SCR_IL_CONFIG    0x0001
#define TM_SCR_IL_RESET     0x0000

#define TM_SCR_MAS_SET      0x0001    /*  !!! new   */
#define TM_SCR_MAS_CLR      0x0000    /*  !!! new   */
#define TM_SCR_MAS_MSK      0x0008    /*  !!! new   */
#define TM_SCR_MAS_OFF      3         /*  !!! new   */

/******************************************************************************
 * Memory Configuration Register: MCR
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int dmy__ : 9;  /* !!! Unsupported bits */
        unsigned int mo    : 2;
        unsigned int qo    : 2;
        unsigned int mcm   : 3;
#else
        unsigned int mcm   : 3;
        unsigned int qo    : 2;
        unsigned int mo    : 2;
        unsigned int dmy__ : 9;  /* !!! Unsupported bits */
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD   w;
} TM_TYPE_MCR;

#define TM_MCM_16K        0
#define TM_MCM_32K        1
#define TM_MCM_64K        2
#define TM_MCM_256K       3
#define TM_MCM_1M         4

#define TM_MCR_MO_3       0x0060
#define TM_MCR_MO_2       0x0040
#define TM_MCR_MO_1       0x0020
#define TM_MCR_MO_0       0x0000

#define TM_MCR_QO_3       0x0018
#define TM_MCR_QO_2       0x0010
#define TM_MCR_QO_1       0x0008
#define TM_MCR_QO_0       0x0000

/******************************************************************************
 * Decoder Register: DR
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
    #ifdef MVBIP
        VOL unsigned int dmy__ :  1; /* !!! Unsupported bits   */
        VOL unsigned int ifs   :  2; /* Interframe Spacing     */
        VOL unsigned int old_rld: 1; /* Diagnostic RLD Info    */
        VOL unsigned int icb   :  1; /* Diagnostic ICB Input   */
        VOL unsigned int b_chg :  1; /* Diagnostic ICB Changed */
        VOL unsigned int ica   :  1; /* Diagnostic ICA Input   */
        VOL unsigned int a_chg :  1; /* Diagnostic ICA Changed */
        VOL unsigned int turbo :  1; /* Non Standard 3MBit MVB */
        VOL unsigned int opto  :  1; /* OGF Mode */
        VOL unsigned int trafo :  1; /* EMD Mode */
        VOL unsigned int wtr   :  1; /* Wait Until Reply Timeout for all
                                        F-Codes */
    #else
        VOL unsigned int dmy__ : 12;  /* !!! Unsupported bits */
    #endif /* MVBIP */
        VOL unsigned int laa   :  1;
        VOL unsigned int rld   :  1;
        VOL unsigned int ls    :  1;
        VOL unsigned int slm   :  1;
#else
        VOL unsigned int slm   :  1;
        VOL unsigned int ls    :  1;
        VOL unsigned int rld   :  1;
        VOL unsigned int laa   :  1;
    #ifdef MVBIP
        VOL unsigned int wtr   :  1; /* Wait Until Reply Timeout for all
                                        F-Codes */
        VOL unsigned int trafo :  1; /* EMD Mode */
        VOL unsigned int opto  :  1; /* OGF Mode */
        VOL unsigned int turbo :  1; /* Non Standard 3MBit MVB */
        VOL unsigned int a_chg :  1; /* Diagnostic ICA Changed */
        VOL unsigned int ica   :  1; /* Diagnostic ICA Input   */
        VOL unsigned int b_chg :  1; /* Diagnostic ICB Changed */
        VOL unsigned int icb   :  1; /* Diagnostic ICB Input   */
        VOL unsigned int old_rld: 1; /* Diagnostic RLD Info    */
        VOL unsigned int ifs   :  2; /* Interframe Spacing     */
        VOL unsigned int dmy__ :  1; /* !!! Unsupported bits   */
    #else
        VOL unsigned int dmy__ : 12; /* !!! Unsupported bits   */
    #endif /* MVBIP */
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_DR;

/* Alternative definition: integer masks */
#define TM_DR_LAA           0x0008
#define TM_DR_RLD           0x0004
#define TM_DR_LS            0x0002
#define TM_DR_SLM           0x0001

/******************************************************************************
 * Sink-Time Supervision Register: STSR
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int interv:  4;
        unsigned int range : 12;
#else
        unsigned int range : 12;
        unsigned int interv:  4;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_STSR;

#define TM_STSR_INTERV_OFF      0
#define TM_STSR_INTERV_1MS      1
#define TM_STSR_INTERV_2MS      2
#define TM_STSR_INTERV_4MS      3
#define TM_STSR_INTERV_8MS      4
#define TM_STSR_INTERV_16MS     5
#define TM_STSR_INTERV_32MS     6
#define TM_STSR_INTERV_64MS     7
#define TM_STSR_INTERV_128MS    8
#define TM_STSR_INTERV_256MS    9

/******************************************************************************
 * Master Register: MR
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int par1  : 1;
        unsigned int par0  : 1;
        unsigned int ea1   : 1;
        unsigned int ea0   : 1;
        unsigned int ec1   : 1;
        unsigned int ec0   : 1;
        unsigned int busy  : 1;
        unsigned int csmf  : 1;
        unsigned int smf   : 2;
        unsigned int smfm  : 1;
        unsigned int size  : 5;
#else
        unsigned int size  : 5;
        unsigned int smfm  : 1;
        unsigned int smf   : 2;
        unsigned int csmf  : 1;
        unsigned int busy  : 1;
        unsigned int ec0   : 1;
        unsigned int ec1   : 1;
        unsigned int ea0   : 1;
        unsigned int ea1   : 1;
        unsigned int par0  : 1;
        unsigned int par1  : 1;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_MR;

/* Alternative definition: integer masks */
#define TM_MR_SMFM      0x0020
#define TM_MR_SMFA      0x0040
#define TM_MR_SMFT      0x0080
#define TM_MR_SMFE      0x00C0
#define TM_MR_CSMF      0x0100
#define TM_MR_BUSY      0x0200
#define TM_MR_EC0       0x0400
#define TM_MR_EC1       0x0800
#define TM_MR_EA0       0x1000
#define TM_MR_EA1       0x2000
#define TM_MR_PAR0      0x4000
#define TM_MR_PAR1      0x8000

#define TM_MR_CSMF_SET   0x0001     /* !!! new */
#define TM_MR_CSMF_MSK   0x0100     /* !!! new */
#define TM_MR_CSMF_OFF   8          /* !!! new */

#define TM_MR_SMFM_SET   0x0001     /* !!! new */
#define TM_MR_SMFM_MSK   0x0020     /* !!! new */
#define TM_MR_SMFM_OFF   5          /* !!! new */

#define TM_MR_SMFX_MSK   0x00DF     /* !!! new */
#define TM_MR_SMFX_OFF   0          /* !!! new */

#define TM_MR_SMFE_SET   0x00C0     /* !!! new */

/* Bit field 'smf' only: Valid values, for both MR and MR2 */

#define TM_MRX_MONE     0;
#define TM_MRX_SMFA     1;
#define TM_MRX_SMFT     2;
#define TM_MRX_SMFE     3;

/******************************************************************************
 * Secondary Master Register: MR2
 *****************************************************************************/
/*
 * Use TM_TYPE_MR, but following bits are ignored:
 *      par1, par0, ea1, ea0, ec1, ec0, busy, csmf
 * Interrupt 0 Registers: IMR0, ISR0, IVR0
 */
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int emf   : 1;
        unsigned int esf   : 1;
        unsigned int dmf   : 1;
        unsigned int dsf   : 1;
        unsigned int amfx  : 1;
        unsigned int mfc   : 1;
        unsigned int sfc   : 1;
        unsigned int rti   : 1;
        unsigned int bti   : 1;
        unsigned int dti7  : 1;
        unsigned int dti6  : 1;
        unsigned int dti5  : 1;
        unsigned int dti4  : 1;
        unsigned int dti3  : 1;
        unsigned int dti2  : 1;
        unsigned int dti1  : 1;
#else
        unsigned int dti1  : 1;
        unsigned int dti2  : 1;
        unsigned int dti3  : 1;
        unsigned int dti4  : 1;
        unsigned int dti5  : 1;
        unsigned int dti6  : 1;
        unsigned int dti7  : 1;
        unsigned int bti   : 1;
        unsigned int rti   : 1;
        unsigned int sfc   : 1;
        unsigned int mfc   : 1;
        unsigned int amfx  : 1;
        unsigned int dsf   : 1;
        unsigned int dmf   : 1;
        unsigned int esf   : 1;
        unsigned int emf   : 1;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_I0;

/* Alternative definition: integer masks */
#define TM_I0_EMF       0x8000
#define TM_I0_ESF       0x4000
#define TM_I0_DMF       0x2000
#define TM_I0_DSF       0x1000
#define TM_I0_AMFX      0x0800
#define TM_I0_MFC       0x0400
#define TM_I0_SFC       0x0200
#define TM_I0_RTI       0x0100
#define TM_I0_BTI       0x0080

#define TM_I0_DTI       0x007F

#define TM_I0_DTI7      0x0040
#define TM_I0_DTI6      0x0020
#define TM_I0_DTI5      0x0010
#define TM_I0_DTI4      0x0008
#define TM_I0_DTI3      0x0004
#define TM_I0_DTI2      0x0002
#define TM_I0_DTI1      0x0001

/******************************************************************************
 * Dispatch Pointer Register: DPR
 *****************************************************************************/
#define TM_DPR_MASK  0xFFFC   /* Lower 2 bits are zero */
/* Interrupt 1 Registers: IMR1, ISR1, IVR1 */

#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int ti2   : 1;
        unsigned int xi3   : 1;
        unsigned int xi2   : 1;
        unsigned int xqe   : 1;
        unsigned int rqe   : 1;
        unsigned int xq1c  : 1;
        unsigned int xq0c  : 1;
        unsigned int rqc   : 1;
        unsigned int fev   : 1;
        unsigned int dmy__ : 4;
        unsigned int ti1   : 1;
        unsigned int xi1   : 1;
        unsigned int xi0   : 1;
#else
        unsigned int xi0   : 1;
        unsigned int xi1   : 1;
        unsigned int ti1   : 1;
        unsigned int dmy__ : 4;
        unsigned int fev   : 1;
        unsigned int rqc   : 1;
        unsigned int xq0c  : 1;
        unsigned int xq1c  : 1;
        unsigned int rqe   : 1;
        unsigned int xqe   : 1;
        unsigned int xi2   : 1;
        unsigned int xi3   : 1;
        unsigned int ti2   : 1;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_I1;

/* Alternative definition: integer masks */
#define TM_I1_TI2       0x8000
#define TM_I1_XI3       0x4000
#define TM_I1_XI2       0x2000
#define TM_I1_XQE       0x1000
#define TM_I1_RQE       0x0800
#define TM_I1_XQ1C      0x0400
#define TM_I1_XQ0C      0x0200
#define TM_I1_RQC       0x0100
#define TM_I1_FEV       0x0080
#define TM_I1_TI1       0x0004
#define TM_I1_XI1       0x0002
#define TM_I1_XI0       0x0001

/******************************************************************************
 * Interrupt Vector Register IVR0/1
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        TM_TYPE_BYTE    iav;
        TM_TYPE_BYTE    vec;
#else
        TM_TYPE_BYTE    vec;
        TM_TYPE_BYTE    iav;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD   w;
} TM_TYPE_IVR;

/* Alternative definition: integer masks */
#define TM_IVR_IAV      0x0100
#define TM_IVR_MASK     0x00FF

/******************************************************************************
 * Device Address Override Key DAOK
 *****************************************************************************/

#define TM_DAOK_DISALBED    0x0000      /* Disabled Status  (Rd)             */
#define TM_DAOK_ENABLE      0x0094      /* Enable override  (Wr)             */
#define TM_DAOK_DISABLE     0x0049      /* Disable override (Wr)             */
#define TM_DAOK_ENABLED     0x00FF      /* Enabled Status   (Rd)             */

/******************************************************************************
 * Timer Control Register: TCR
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int dmy__ : 10;
        unsigned int rs2   : 1;
        unsigned int ta2   : 1;
        unsigned int dmy_0 : 1;
        unsigned int xsyn  : 1;
        unsigned int rs1   : 1;
        unsigned int ta1   : 1;
#else
        unsigned int ta1   : 1;
        unsigned int rs1   : 1;
        unsigned int xsyn  : 1;
        unsigned int dmy_0 : 1;
        unsigned int ta2   : 1;
        unsigned int rs2   : 1;
        unsigned int dmy__ : 10;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD   w;
} TM_TYPE_TCR;

/* Alternative definition: integer masks */
#define TM_TCR_RS2      0x0020
#define TM_TCR_TA2      0x0010
#define TM_TCR_XSYN     0x0004
#define TM_TCR_RS1      0x0002
#define TM_TCR_TA1      0x0001

/******************************************************************************
 * Data Structure: Internal Registers (MVBC)
 *****************************************************************************/
#pragma pack(2)
typedef VOL struct {
    VOL TM_TYPE_SCR     scr;   /* + 0x00 */
    VOL TM_TYPE_WORD    dmy__02;
    VOL TM_TYPE_MCR     mcr;   /* + 0x04 */
    VOL TM_TYPE_WORD    dmy__06;
    VOL TM_TYPE_DR      dr;    /* + 0x08 */
    VOL TM_TYPE_WORD    dmy__0A;
    VOL TM_TYPE_STSR    stsr;  /* + 0x0C */
    VOL TM_TYPE_WORD    dmy__0E;

    VOL TM_TYPE_WORD    fc;    /* + 0x10 */
    VOL TM_TYPE_WORD    dmy__12;
    VOL TM_TYPE_WORD    ec;    /* + 0x14 */
    VOL TM_TYPE_WORD    dmy__16;
    VOL TM_TYPE_WORD    mfr;   /* + 0x18 */
    VOL TM_TYPE_WORD    dmy__1A;
    VOL TM_TYPE_WORD    mfre;  /* + 0x1C */
    VOL TM_TYPE_WORD    dmy__1E;

    VOL TM_TYPE_MR      mr;    /* + 0x20 */
    VOL TM_TYPE_WORD    dmy__22;
    VOL TM_TYPE_MR      mr2;   /* + 0x24 */
    VOL TM_TYPE_WORD    dmy__26;
    VOL TM_TYPE_WORD    dpr;   /* + 0x28 */
    VOL TM_TYPE_WORD    dmy__2A;
    VOL TM_TYPE_WORD    dpr2;  /* + 0x2C */
    VOL TM_TYPE_WORD    dmy__2E;

    VOL TM_TYPE_I0      ipr0;  /* + 0x30 */
    VOL TM_TYPE_WORD    dmy__32;
    VOL TM_TYPE_I1      ipr1;  /* + 0x34 */
    VOL TM_TYPE_WORD    dmy__36;
    VOL TM_TYPE_I0      imr0;  /* + 0x38 */
    VOL TM_TYPE_WORD    dmy__3A;
    VOL TM_TYPE_I1      imr1;  /* + 0x3C */
    VOL TM_TYPE_WORD    dmy__3E;

    VOL TM_TYPE_I0      isr0;  /* + 0x40 */
    VOL TM_TYPE_WORD    dmy__42;
    VOL TM_TYPE_I1      isr1;  /* + 0x44 */
    VOL TM_TYPE_WORD    dmy__46;
    VOL TM_TYPE_IVR     ivr0;  /* + 0x48 */
    VOL TM_TYPE_WORD    dmy__4A;
    VOL TM_TYPE_IVR     ivr1;  /* + 0x4C */
    VOL TM_TYPE_WORD    dmy__4E;

    VOL TM_TYPE_WORD    dmy__50[4];
    VOL TM_TYPE_WORD    daor;  /* + 0x58 */

    VOL TM_TYPE_WORD    dmy__5A;
    VOL TM_TYPE_WORD    daok;  /* + 0x5C */
    VOL TM_TYPE_WORD    dmy__5E;
    VOL TM_TYPE_TCR     tcr;   /* + 0x60 */
    VOL TM_TYPE_WORD    dmy__62[7];

    VOL TM_TYPE_WORD    tr1;   /* + 0x70 */
    VOL TM_TYPE_WORD    dmy__72;
    VOL TM_TYPE_WORD    tr2;   /* + 0x74 */
    VOL TM_TYPE_WORD    dmy__76;
    VOL TM_TYPE_WORD    tc1;   /* + 0x78 */
    VOL TM_TYPE_WORD    dmy__7A;
    VOL TM_TYPE_WORD    tc2;   /* + 0x7C */
    VOL TM_TYPE_WORD    dmy__7E;
} TM_TYPE_INT_REGS;

/******************************************************************************
 * Data Structure: Entire Service Area
 *****************************************************************************/
#pragma pack(2)
typedef struct {
    TM_TYPE_DATA     pp_data[ 8];  /* !!! Only 1st  4 of them are used       */
    TM_TYPE_PCS      pp_pcs [32];  /* !!! Only 1st 16 of them are used       */
    TM_TYPE_WORD     mfs;          /* Master Frame Slot                      */
    TM_TYPE_WORD     dmy__1[7];    /* Vacancy between MFS and QDT            */
    TM_TYPE_QDT      qdt;          /* Queue Descriptor Table                 */
    TM_TYPE_WORD     dmy__2[53];   /* Vacancy between QDT and 1st reg.       */
    TM_TYPE_INT_REGS int_regs;     /* !!! Located inside MVBC                */
} TM_TYPE_SERVICE_AREA;


/* Location of physical ports, needed for pp_pcs and pp_data                 */

#define TM_PP_FC8     0x0   /* Mastership Offer Source Port                  */
#define TM_PP_EFS     0x1   /* Event Frame Sink Port                         */
#define TM_PP_EF0     0x4   /* Event Frame Source Port for Ev. Type = 0      */
#define TM_PP_EF1     0x5   /* Event Frame Source Port for Ev. Type = 1      */

#define TM_PP_MOS     0x6   /* Mastership Offser Sink  Port                  */
#define TM_PP_FC15    0x7   /* Device Status Report Source Port              */

#define TM_PP_MSRC    0x8   /* Message Source Port                           */
#define TM_PP_TSRC    0x8   /* Test    Source Port                           */
#define TM_PP_MSNK    0xC   /* Message Sink   Port                           */
#define TM_PP_TSNK    0xC   /* Test    Sink   Port                           */

/* Location of physical ports, needed for pp_pcs and pp_data */
#define TM_SRV_AREA_SIZE            1024
#define TM_SRV_AREA_SIZE_NO_REGS    (1024-128)

/******************************************************************************
 * Locations and sizes of the different TM areas w.r.t MCM
 *****************************************************************************/
#define TM_PORT_COUNT      4096  /* Applies to all ports */

#define TM_OFFSET_COUNT    5

/*  Parameters for MCM     = 0 (16K)  1 (32K)  2 (64K)  3 (256K)  4 (1M)  */
#define TM_LA_PCS_OFFSETS  { 0x3000L, 0x3000L, 0xC000L, 0x30000L, 0x30000L }
#define TM_DA_PCS_OFFSETS  { 0x0L,    0x7000L, 0xF000L, 0x4000L , 0x38000L }

#define TM_LA_DATA_OFFSETS { 0x1000L, 0x1000L, 0x4000L, 0x10000L, 0x10000L }
#define TM_MEM_AFTER_SA    { 0x0L,    0x0L,    0x0L,    0x30000L, 0x70000L }

#define TM_DA_DATA_OFFSETS { 0x0L,    0x5000L, 0xE000L, 0x38000L, 0x40000L }
#define TM_LA_FRCE_OFFSETS { 0x2000L, 0x2000L, 0x8000L, 0x20000L, 0x20000L }

#define TM_LA_PIT_OFFSETS  { 0x0000L, 0x0000L, 0x0000L, 0x0000L , 0x0000L  }
#define TM_DA_PIT_OFFSETS  { 0x0L,    0x4000L, 0x2000L, 0x2000L , 0x2000L  }

#define TM_SERVICE_OFFSETS { 0x3C00L, 0x7C00L, 0xFC00L, 0xFC00L , 0xFC00L  }

#define TM_PIT_BYTE_SIZES  { 4096,    4096,    8192,    8192,     8192     }
#define TM_LA_PORT_COUNTS  { 256,     256,     1024,    4096,     4096     }
#define TM_DA_PORT_COUNTS  { 0,       256,     256,     2048,     4096     }

#define TM_MEMORY_SIZES    {16*1024l, 32*1024l, 64*1024l, 256*1024l, 1024*1024l}

/* Usage example:     unsigned long la_pcs_offsets[] = TM_LA_PCS_OFFSETS;    */


/******************************************************************************
 *
 * Data Structure Usage
 *
 ******************************************************************************
 *
 * Internal Registers:
 *
 * srv_area->int_regs.scr.b.il = 0;    -- Bit type access to Init. Level
 * srv_area->int_regs.mcr.w    = 0;    -- Word type access
 *
 * Physical Ports (PCS):
 *
 * srv_area->pp_pcs[TM_PP_EF0].word0.b.f_code = 0;  -- Bit type access
 * srv_area->pp_pcs[TM_PP_MOS].tack = 0;            -- Word type access
 *
 * Physical Ports (Data):
 *
 * srv_area->pp_data[TM_PP_EF0].word0.b.f_code = 0; -- Bit type access
 * srv_area->pp_data[TM_PP_MOS].w[i+4*vp] = 0;      -- Word type access
 * srv_area->pp_data[TM_PP_MOS].b[i+8*vp] = 0;      -- Byte type access
 *
 * Queue Descriptor Table (QDT):
 *
 * srv_area->qdt.xmit_q[0] = 0;                     -- Transmit Queues
 * srv_area->qdt.rcve_q    = 0;                     -- Receive Queues
 *
 * Master Frame Slot (MFS):
 *
 * srv_area->mfs = 0;                               -- 1 Master Frame
 *
 *****************************************************************************/


/******************************************************************************
 * Data Structure: Frame Contents
 *****************************************************************************/

/******************************************************************************
 * Device Status Word: DSW
 *****************************************************************************/
#pragma pack(2)
typedef union {
    struct {
#ifdef BIGENDIAN
        unsigned int devtype  : 4;
        unsigned int reserved : 4;
        unsigned int laa      : 1;
        unsigned int rld      : 1;
        unsigned int ssd      : 1;
        unsigned int sdd      : 1;
        unsigned int spd      : 1;
        unsigned int frc      : 1;
        unsigned int dnr      : 1;
        unsigned int ser      : 1;
#else
        unsigned int ser      : 1;
        unsigned int dnr      : 1;
        unsigned int frc      : 1;
        unsigned int spd      : 1;
        unsigned int sdd      : 1;
        unsigned int ssd      : 1;
        unsigned int rld      : 1;
        unsigned int laa      : 1;
        unsigned int reserved : 4;
        unsigned int devtype  : 4;
#endif /* BIGENDIAN */
    } b;
    TM_TYPE_WORD w;
} TM_TYPE_DSW;

/* Alternative definition: integer masks */

#define TM_DSW_DEVTYPE      0xF000

#define TM_DSW_CLASS_1      0xF000
#define TM_DSW_CLASS_23     0x0000
#define TM_DSW_BRIDGE       0x8000
#define TM_DSW_BUS_ADM      0x4000

#define TM_DSW_RESERVED     0x0F00
#define TM_DSW_LAA          0x0080
#define TM_DSW_RLD          0x0040
#define TM_DSW_SSD          0x0020
#define TM_DSW_SDD          0x0010
#define TM_DSW_SPD          0x0008
#define TM_DSW_FRC          0x0004
#define TM_DSW_DNR          0x0002
#define TM_DSW_SER          0x0001

/******************************************************************************
 * Universal Constants
 *****************************************************************************/
/* Function Code, applies to both PCS Word 0 and Master Frames               */
#define    W_FC0        0x0000    /* F-Code 0                                */
#define    W_FC1        0x1000    /* F-Code 1                                */
#define    W_FC2        0x2000    /* F-Code 2                                */
#define    W_FC3        0x3000    /* F-Code 3                                */
#define    W_FC4        0x4000    /* F-Code 4                                */
#define    W_FC5        0x5000    /* F-Code 5                                */
#define    W_FC6        0x6000    /* F-Code 6                                */
#define    W_FC7        0x7000    /* F-Code 7                                */
#define    W_FC8        0x8000    /* F-Code 8                                */
#define    W_FC9        0x9000    /* F-Code 9                                */
#define    W_FC10       0xa000    /* F-Code 10                               */
#define    W_FC11       0xb000    /* F-Code 11                               */
#define    W_FC12       0xc000    /* F-Code 12                               */
#define    W_FC13       0xd000    /* F-Code 13                               */
#define    W_FC14       0xe000    /* F-Code 14                               */
#define    W_FC15       0xf000    /* F-Code 15                               */

/* Communication Mode (CM), found in Master Frame with F-Code = 9            */
#define    CM_PT_TO_PT  0x1000    /* Single-Cast                             */
#define    CM_BROADCAST 0xF000    /* Broadcast                               */
#define    CM_MASK      0xF000    /* Communication Mode Mask                 */

/* Event Modes (EM), found in Master Frame with F-Code = 9                   */
#define    EM_0        0x0000
#define    EM_1        0x0100
#define    EM_2        0x0200
#define    EM_3        0x0300
#define    EM_MASK     0x0F00     /* Event Mode Mask                         */

/* Event Types (ET), found in Master Frame with F-Code = 9                   */
#define    ET_0        0x0000
#define    ET_1        0x0010
#define    ET_MASK     0x00F0    /* Event Type Mask                          */

/* Device Group Addresses (DGA), found in Master Frame with F-Code = 13      */

#define    DGA_ALL_DEV 0xFFE     /* All  Devices selected                    */
#define   DGA_EVEN_DEV 0xFFC     /* Even Devices selected                    */
#define    DGA_ODD_DEV 0xFFD     /* Odd  Devices selected                    */
#define    DGA_MASK    0xFFF     /* Device Group Address Mask                */


/******************************************************************************
 * Traffic Memory Layouts
 *****************************************************************************/
#pragma pack(2)
typedef struct {                 /* Mem Configuration Mode 0 (16 KB)         */
    TM_TYPE_BYTE la_pit  [4096]; /* LA Port Index Table 4096 Ports x 1 Byte  */
    TM_TYPE_WORD la_data [2048]; /* LA Data Area 256 docks x 2 Pg * 4 Words  */
    TM_TYPE_WORD la_frce [2048]; /* LA Frce Area 256 docks x 2 Pg * 4 Words  */
    TM_TYPE_PCS  la_pcs   [256]; /* LA PCS  Area 256 Ports x 4 Words         */
    TM_TYPE_WORD free_1   [512]; /* Unassigned Memory                        */
    TM_TYPE_SERVICE_AREA sa;     /* Service Area 1KByte                      */
} TM_LAYOUT_16K;

#pragma pack(2)
typedef struct {                 /* Mem Configuration Mode 1 (32 KB)         */
    TM_TYPE_BYTE la_pit  [4096]; /* LA Port Index Table 4096 Ports x 1 Byte  */
    TM_TYPE_WORD la_data [2048]; /* LA Data Area 256 docks x 2 Pg * 4 Words  */
    TM_TYPE_WORD la_frce [2048]; /* LA Frce Area 256 docks x 2 Pg * 4 Words  */
    TM_TYPE_PCS  la_pcs   [256]; /* LA PCS  Area 256 Ports x 4 Words         */
    TM_TYPE_WORD free_1   [512]; /* Unassigned Memory 1KByte                 */

    TM_TYPE_BYTE da_pit  [4096]; /* DA Port Index Table 4096 Ports x 1 Byte  */
    TM_TYPE_WORD da_data [2048]; /* DA Data Area 256 docks x 2 Pg * 4 Words  */
    TM_TYPE_WORD free_2  [2048]; /* Unassigned Memory 4KByte                 */
    TM_TYPE_PCS  da_pcs   [256]; /* DA PCS  Area 256 Ports x 4 Words         */
    TM_TYPE_WORD free_3   [512]; /* Unassigned Memory 1KByte                 */
    TM_TYPE_SERVICE_AREA sa;     /* Service Area 1KByte                      */
} TM_LAYOUT_32K;

#pragma pack(2)
typedef struct {                 /* Mem Configuration Mode 2 (64 KB)         */
    TM_TYPE_WORD la_pit  [4096]; /* LA Port Index Table 4096 Ports x 1 Word  */
    TM_TYPE_WORD da_pit  [4096]; /* DA Port Index Table 4096 Ports x 1 Word  */
    TM_TYPE_WORD la_data [8192]; /* LA Data Area 1024 docks x 2 Pg * 4 Words */
    TM_TYPE_WORD la_frce [8192]; /* LA Frce Area 1024 docks x 2 Pg * 4 Words */
    TM_TYPE_PCS  la_pcs  [1024]; /* LA PCS  Area 1024 Ports x 4 Words        */
    TM_TYPE_WORD da_data [2048]; /* DA Data Area 256 docks x 2 Pg * 4 Words  */
    TM_TYPE_PCS  da_pcs   [256]; /* DA PCS  Area 256 Ports x 4 Words         */
    TM_TYPE_WORD free_1   [512]; /* Unassigned Memory 1KByte                 */
    TM_TYPE_SERVICE_AREA sa;     /* Service Area 1KByte                      */
} TM_LAYOUT_64K;

#pragma pack(2)
typedef struct {                 /* Mem Configuration Mode 3 (256 KB)        */
    TM_TYPE_WORD la_pit  [4096]; /* LA Port Index Table 4096 Ports x 1 Word  */
    TM_TYPE_WORD da_pit  [4096]; /* DA Port Index Table 4096 Ports x 1 Word  */
    TM_TYPE_PCS  da_pcs  [2048]; /* DA PCS  Area 2048 Ports x 4 Words        */
    TM_TYPE_WORD free_1 [15872]; /* Unassigned Memory 31KByte                */
    TM_TYPE_SERVICE_AREA sa;     /* Service Area 1KByte                      */

    TM_TYPE_WORD la_data[32768]; /* LA Data Area 4096 docks x 2 Pg * 4 Words */
    TM_TYPE_WORD la_frce[32768]; /* LA Frce Area 4096 docks x 2 Pg * 4 Words */
    TM_TYPE_PCS  la_pcs  [4096]; /* LA PCS  Area 4096 Ports x 4 Words        */
    TM_TYPE_WORD da_data [2048]; /* DA Data Area 2048 docks x 2 Pg * 4 Words */
} TM_LAYOUT_256K;

#pragma pack(2)
typedef struct {                 /* Mem Configuration Mode 4 (1024 KB)       */
    TM_TYPE_WORD la_pit  [4096]; /* LA Port Index Table 4096 Ports x 1 Word  */
    TM_TYPE_WORD da_pit  [4096]; /* DA Port Index Table 4096 Ports x 1 Word  */
    TM_TYPE_WORD free_1 [24064]; /* Unassigned Memory 47KByte                */
    TM_TYPE_SERVICE_AREA sa;     /* Service Area 1KByte                      */

    TM_TYPE_WORD la_data[32768]; /* LA Data Area 4096 docks x 2 Pg * 4 Words */
    TM_TYPE_WORD la_frce[32768]; /* LA Frce Area 4096 docks x 2 Pg * 4 Words */
    TM_TYPE_PCS  la_pcs  [4096]; /* LA PCS  Area 4096 Ports x 4 Words        */
    TM_TYPE_PCS  da_pcs  [4096]; /* DA PCS  Area 4096 Ports x 4 Words        */
    TM_TYPE_WORD da_data [4096]; /* DA Data Area 2048 docks x 2 Pg * 4 Words */
    TM_TYPE_WORD free_2[360448]; /* Unassigned Memory 704KByte !!!           */
} TM_LAYOUT_1024K;

#endif /* MVBC_H */
