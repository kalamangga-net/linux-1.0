/* 
 * NCR 5380 defines
 *
 * Copyright 1993, Drew Eckhardt
 *	Visionary Computing
 *	(Unix consulting and custom programming)
 * 	drew@colorado.edu
 *      +1 (303) 666-5836
 *
 * DISTRIBUTION RELEASE 4
 *
 * For more information, please consult 
 *
 * NCR 5380 Family
 * SCSI Protocol Controller
 * Databook
 * NCR Microelectronics
 * 1635 Aeroplaza Drive
 * Colorado Springs, CO 80916
 * 1+ (719) 578-3400
 * 1+ (800) 334-5454
 */

/*
 * $Log: NCR5380.h,v $
 * Revision 1.3  1994/01/19  05:24:40  drew
 * Added support for TCR LAST_BYTE_SENT bit.
 *
 * Revision 1.3  1994/01/19  05:24:40  drew
 * Added support for TCR LAST_BYTE_SENT bit.
 *
 * Revision 1.2  1994/01/15  06:14:11  drew
 * REAL DMA support, bug fixes.
 *
 * Revision 1.1  1994/01/15  06:00:54  drew
 * Initial revision
 */

#ifndef NCR5380_H
#define NCR5380_H

#define NCR5380_PUBLIC_RELEASE 4

#define NDEBUG_ARBITRATION	0x1
#define NDEBUG_AUTOSENSE	0x2
#define NDEBUG_DMA		0x4
#define NDEBUG_HANDSHAKE	0x8
#define NDEBUG_INFORMATION	0x10
#define NDEBUG_INIT		0x20
#define NDEBUG_INTR		0x40
#define NDEBUG_LINKED		0x80
#define NDEBUG_MAIN		0x100
#define NDEBUG_NO_DATAOUT	0x200
#define NDEBUG_NO_WRITE		0x400
#define NDEBUG_PIO		0x800
#define NDEBUG_PSEUDO_DMA	0x1000
#define NDEBUG_QUEUES		0x2000
#define NDEBUG_RESELECTION	0x4000
#define NDEBUG_SELECTION	0x8000
#define NDEBUG_USLEEP		0x10000
#define NDEBUG_LAST_BYTE_SENT	0x20000

/* 
 * The contents of the OUTPUT DATA register are asserted on the bus when
 * either arbitration is occuring or the phase-indicating signals (
 * IO, CD, MSG) in the TARGET COMMAND register and the ASSERT DATA
 * bit in the INTITIATOR COMMAND register is set.
 */

#define OUTPUT_DATA_REG         0       /* wo DATA lines on SCSI bus */
#define CURRENT_SCSI_DATA_REG   0       /* ro same */

#define INITIATOR_COMMAND_REG	1	/* rw */
#define ICR_ASSERT_RST		0x80	/* rw Set to assert RST  */
#define ICR_ARBITRATION_PROGRESS 0x40	/* ro Indicates arbitration complete */
#define ICR_TRI_STATE		0x40	/* wo Set to tri-state drivers */
#define ICR_ARBITRATION_LOST	0x20	/* ro Indicates arbitration lost */
#define ICR_DIFF_ENABLE		0x20	/* wo Set to enable diff. drivers */
#define ICR_ASSERT_ACK		0x10	/* rw ini Set to assert ACK */
#define ICR_ASSERT_BSY		0x08	/* rw Set to assert BSY */
#define ICR_ASSERT_SEL 		0x04	/* rw Set to assert SEL */
#define ICR_ASSERT_ATN		0x02	/* rw Set to assert ATN */
#define ICR_ASSERT_DATA		0x01	/* rw SCSI_DATA_REG is asserted */

#ifdef DIFFERENTIAL
#define ICR_BASE		ICR_DIFF_ENABLE
#else
#define ICR_BASE		0
#endif

#define MODE_REG		2
/*
 * Note : BLOCK_DMA code will keep DRQ asserted for the duration of the 
 * transfer, causing the chip to hog the bus.  You probably don't want 
 * this.
 */
#define MR_BLOCK_DMA_MODE	0x80	/* rw block mode DMA */
#define MR_TARGET		0x40	/* rw target mode */
#define MR_ENABLE_PAR_CHECK   0x20	/* rw enable parity checking */
#define MR_ENABLE_PAR_INTR	0x10	/* rw enable bad parity interrupt */
#define MR_ENABLE_EOP_INTR	0x08	/* rw enabble eop interrupt */
#define MR_MONITOR_BSY	0x04	/* rw enable int on unexpected bsy fail */
#define MR_DMA_MODE		0x02	/* rw DMA / pseudo DMA mode */
#define MR_ARBITRATE		0x01	/* rw start arbitration */

#ifdef PARITY
#define MR_BASE			MR_ENABLE_PAR_CHECK
#else
#define MR_BASE			0
#endif

#define TARGET_COMMAND_REG	3
#define TCR_LAST_BYTE_SENT	0x80	/* ro DMA done */
#define TCR_ASSERT_REQ		0x08	/* tgt rw assert REQ */
#define TCR_ASSERT_MSG		0x04	/* tgt rw assert MSG */
#define TCR_ASSERT_CD		0x02	/* tgt rw assert CD */
#define TCR_ASSERT_IO		0x01	/* tgt rw assert IO */

#define STATUS_REG		4	/* ro */
/*
 * Note : a set bit indicates an active signal, driven by us or another 
 * device.
 */
#define SR_RST			0x80	
#define SR_BSY			0x40
#define SR_REQ			0x20
#define SR_MSG			0x10
#define SR_CD			0x08
#define SR_IO			0x04
#define SR_SEL			0x02
#define SR_DBP			0x01

/*
 * Setting a bit in this register will cause an interrupt to be generated when 
 * BSY is false and SEL true and this bit is asserted  on the bus.
 */
#define SELECT_ENABLE_REG	4	/* wo */

#define BUS_AND_STATUS_REG	5	/* ro */
#define BASR_END_DMA_TRANSFER	0x80	/* ro set on end of transfer */
#define BASR_DRQ		0x40	/* ro mirror of DRQ pin */
#define BASR_PARITY_ERROR	0x20	/* ro parity error detected */
#define BASR_IRQ		0x10	/* ro mirror of IRQ pin */
#define BASR_PHASE_MATCH	0x08	/* ro Set when MSG CD IO match TCR */
#define BASR_BUSY_ERROR		0x04	/* ro Unexpected change to inactive state */
#define BASR_ATN 		0x02	/* ro BUS status */
#define BASR_ACK		0x01	/* ro BUS status */

/* Write any value to this register to start a DMA send */
#define START_DMA_SEND_REG	5	/* wo */

/* 
 * Used in DMA transfer mode, data is latched from the SCSI bus on
 * the falling edge of REQ (ini) or ACK (tgt)
 */
#define INPUT_DATA_REG			6	/* ro */

/* Write any value to this register to start a DMA recieve */
#define START_DMA_TARGET_RECIEVE_REG	6	/* wo */

/* Read this register to clear interrupt conditions */
#define RESET_PARITY_INTERRUPT_REG	7	/* ro */

/* Write any value to this register to start an ini mode DMA recieve */
#define START_DMA_INITIATOR_RECIEVE_REG 7	/* wo */

/* Note : PHASE_* macros are based on the values of the STATUS register */
#define PHASE_MASK 	(SR_MSG | SR_CD | SR_IO)

#define PHASE_DATAOUT	0
#define PHASE_DATAIN	SR_IO
#define PHASE_CMDOUT	SR_CD
#define PHASE_STATIN	(SR_CD | SR_IO)
#define PHASE_MSGOUT	(SR_MSG | SR_CD)
#define PHASE_MSGIN	(SR_MSG | SR_CD | SR_IO)
#define PHASE_UNKNOWN	0xff

/* 
 * Convert status register phase to something we can use to set phase in 
 * the target register so we can get phase mismatch interrupts on DMA 
 * transfers.
 */
 
#define PHASE_SR_TO_TCR(phase) ((phase) >> 2)	

/*
 * The internal should_disconnect() function returns these based on the 
 * expected length of a disconnect if a device supports disconnect/
 * reconnect.
 */

#define DISCONNECT_NONE		0
#define DISCONNECT_TIME_TO_DATA	1
#define DISCONNECT_LONG		2

/* 
 * These are "special" values for the tag parameter passed to NCR5380_select.
 */

#define TAG_NEXT	-1 	/* Use next free tag */
#define TAG_NONE	-2	/* 
				 * Establish I_T_L nexus instead of I_T_L_Q
				 * even on SCSI-II devices.
				 */

/*
 * These are "special" values for the irq and dma_channel fields of the 
 * Scsi_Host structure
 */

#define IRQ_NONE	255
#define DMA_NONE	255
#define IRQ_AUTO	254
#define DMA_AUTO	254

#define FLAG_HAS_LAST_BYTE_SENT		1	/* NCR53c81 or better */
#define FLAG_CHECK_LAST_BYTE_SENT	2	/* Only test once */

#ifndef ASM
struct NCR5380_hostdata {
    NCR5380_implementation_fields;		/* implmenentation specific */
    unsigned char id_mask, id_higher_mask;	/* 1 << id, all bits greater */
    volatile unsigned char busy[8];		/* index = target, bit = lun */
#if defined(REAL_DMA) || defined(REAL_DMA_POLL)
    volatile int dma_len;			/* requested length of DMA */
#endif
    volatile unsigned char last_message;	/* last message OUT */
    volatile Scsi_Cmnd *connected;		/* currently connected command */
    volatile Scsi_Cmnd *issue_queue;		/* waiting to be issued */
    volatile Scsi_Cmnd *disconnected_queue;	/* waiting for reconnect */
    int flags;
#ifdef USLEEP
    unsigned long time_expires;			/* in jiffies, set prior to sleeping */
    struct Scsi_Host *next_timer;
#endif
};

#ifdef __KERNEL__
static struct Scsi_Host *first_instance;		/* linked list of 5380's */

#if defined(AUTOPROBE_IRQ)
static int NCR5380_probe_irq (struct Scsi_Host *instance, int possible);
#endif
static void NCR5380_init (struct Scsi_Host *instance);
static void NCR5380_information_transfer (struct Scsi_Host *instance);
static void NCR5380_intr (int irq);
static void NCR5380_main (void);
static void NCR5380_print_options (struct Scsi_Host *instance);
#ifndef NCR5380_abort
static
#endif
int NCR5380_abort (Scsi_Cmnd *cmd, int code);
#ifndef NCR5380_reset
static
#endif
int NCR5380_reset (Scsi_Cmnd *cmd);
#ifndef NCR5380_queue_command
static 
#endif
int NCR5380_queue_command (Scsi_Cmnd *cmd, void (*done)(Scsi_Cmnd *));


static void NCR5380_reselect (struct Scsi_Host *instance);
static int NCR5380_select (struct Scsi_Host *instance, Scsi_Cmnd *cmd, int tag);
#if defined(PSEUDO_DMA) || defined(REAL_DMA) || defined(REAL_DMA_POLL)
static int NCR5380_transfer_dma (struct Scsi_Host *instance,
        unsigned char *phase, int *count, unsigned char **data);
#endif
static int NCR5380_transfer_pio (struct Scsi_Host *instance,
        unsigned char *phase, int *count, unsigned char **data);

#if (defined(REAL_DMA) || defined(REAL_DMA_POLL)) && defined(i386)
static __inline__ int NCR5380_i386_dma_setup (struct Scsi_Host *instance,
	unsigned char *ptr, unsigned int count, unsigned char mode) {
    unsigned limit;

    if (instance->dma_channel <=3) {
	if (count > 65536)
	    count = 65536;
	limit = 65536 - (((unsigned) ptr) & 0xFFFF);
    } else {
	if (count > 65536 * 2) 
	    count = 65536 * 2;
	limit = 65536* 2 - (((unsigned) ptr) & 0x1FFFF);
    }

    if (count > limit) count = limit;

    if ((count & 1) || (((unsigned) ptr) & 1))
	panic ("scsi%d : attmpted unaligned DMA transfer\n", instance->host_no);
    cli();
    disable_dma(instance->dma_channel);
    clear_dma_ff(instance->dma_channel);
    set_dma_addr(instance->dma_channel, (unsigned int) ptr);
    set_dma_count(instance->dma_channel, count);
    set_dma_mode(instance->dma_channel, mode);
    enable_dma(instance->dma_channel);
    sti();
    return count;
}

static __inline__ int NCR5380_i386_dma_write_setup (struct Scsi_Host *instance,
    unsigned char *src, unsigned int count) {
    return NCR5380_i386_dma_setup (instance, src, count, DMA_MODE_WRITE);
}

static __inline__ int NCR5380_i386_dma_read_setup (struct Scsi_Host *instance,
    unsigned char *src, unsigned int count) {
    return NCR5380_i386_dma_setup (instance, src, count, DMA_MODE_READ);
}

static __inline__ int NCR5380_i386_dma_residual (struct Scsi_Host *instance) {
    register int tmp;
    cli();
    clear_dma_ff(instance->dma_channel);
    tmp = get_dma_residue(instance->dma_channel);
    sti();
    return tmp;
}
#endif /* defined(REAL_DMA) && defined(i386)  */
#endif __KERNEL_
#endif /* ndef ASM */
#endif /* NCR5380_H */
