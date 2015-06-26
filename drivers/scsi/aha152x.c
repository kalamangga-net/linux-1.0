/* aha152x.c -- Adaptec AHA-152x driver
 * Author: Juergen E. Fischer, fischer@server.et-inf.fho-emden.de
 * Copyright 1993 Juergen E. Fischer
 *
 *
 * This driver is based on
 *   fdomain.c -- Future Domain TMC-16x0 driver
 * which is
 *   Copyright 1992, 1993 Rickard E. Faith (faith@cs.unc.edu)
 *

 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.

 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 
 *
 * $Id: aha152x.c,v 0.101 1993/12/13 01:16:27 root Exp $
 *

 * $Log: aha152x.c,v $
 * Revision 0.101  1993/12/13  01:16:27  root
 * - fixed STATUS phase (non-GOOD stati were dropped sometimes;
 *   fixes problems with CD-ROM sector size detection & media change)
 *
 * Revision 0.100  1993/12/10  16:58:47  root
 * - fix for unsuccessful selections in case of non-continuous id assignments
 *   on the scsi bus.
 *
 * Revision 0.99  1993/10/24  16:19:59  root
 * - fixed DATA IN (rare read errors gone)
 *
 * Revision 0.98  1993/10/17  12:54:44  root
 * - fixed some recent fixes (shame on me)
 * - moved initialization of scratch area to aha152x_queue
 *
 * Revision 0.97  1993/10/09  18:53:53  root
 * - DATA IN fixed. Rarely left data in the fifo.
 *
 * Revision 0.96  1993/10/03  00:53:59  root
 * - minor changes on DATA IN
 *
 * Revision 0.95  1993/09/24  10:36:01  root
 * - change handling of MSGI after reselection
 * - fixed sti/cli
 * - minor changes
 *
 * Revision 0.94  1993/09/18  14:08:22  root
 * - fixed bug in multiple outstanding command code
 * - changed detection
 * - support for kernel command line configuration
 * - reset corrected
 * - changed message handling
 *
 * Revision 0.93  1993/09/15  20:41:19  root
 * - fixed bugs with multiple outstanding commands
 *
 * Revision 0.92  1993/09/13  02:46:33  root
 * - multiple outstanding commands work (no problems with IBM drive)
 *
 * Revision 0.91  1993/09/12  20:51:46  root
 * added multiple outstanding commands
 * (some problem with this $%&? IBM device remain)
 *
 * Revision 0.9  1993/09/12  11:11:22  root
 * - corrected auto-configuration
 * - changed the auto-configuration (added some '#define's)
 * - added support for dis-/reconnection
 *
 * Revision 0.8  1993/09/06  23:09:39  root
 * - added support for the drive activity light
 * - minor changes
 *
 * Revision 0.7  1993/09/05  14:30:15  root
 * - improved phase detection
 * - now using the new snarf_region code of 0.99pl13
 *
 * Revision 0.6  1993/09/02  11:01:38  root
 * first public release; added some signatures and biosparam()
 *
 * Revision 0.5  1993/08/30  10:23:30  root
 * fixed timing problems with my IBM drive
 *
 * Revision 0.4  1993/08/29  14:06:52  root
 * fixed some problems with timeouts due incomplete commands
 *
 * Revision 0.3  1993/08/28  15:55:03  root
 * writing data works too.  mounted and worked on a dos partition
 *
 * Revision 0.2  1993/08/27  22:42:07  root
 * reading data works.  Mounted a msdos partition.
 *
 * Revision 0.1  1993/08/25  13:38:30  root
 * first "damn thing doesn't work" version
 *
 * Revision 0.0  1993/08/14  19:54:25  root
 * empty function bodies; detect() works.
 *

 **************************************************************************


 
 DESCRIPTION:

 This is the Linux low-level SCSI driver for Adaptec AHA-1520/1522
 SCSI host adapters.


 PER-DEFINE CONFIGURABLE OPTIONS:

 AUTOCONF       : use configuration the controller reports (only 152x)
 IRQ            : override interrupt channel (9,10,11 or 12) (default 11)
 SCSI_ID        : override scsiid of AIC-6260 (0-7) (default 7)
 RECONNECT      : override target dis-/reconnection/multiple outstanding commands
 SKIP_BIOSTEST  : Don't test for BIOS signature (AHA-1510 or disabled BIOS)
 PORTBASE       : Force port base. Don't try to probe


 LILO COMMAND LINE OPTIONS:

 aha152x=<PORTBASE>,<IRQ>,<SCSI-ID>,<RECONNECT>

 The normal configuration can be overridden by specifying a command line.
 When you do this, the BIOS test is skipped. Entered values have to be
 valid (known). Don't use values that aren't support under normal operation.
 If you think that you need other value: contact me.


 REFERENCES USED:

 "AIC-6260 SCSI Chip Specification", Adaptec Corporation.

 "SCSI COMPUTER SYSTEM INTERFACE - 2 (SCSI-2)", X3T9.2/86-109 rev. 10h

 "Writing a SCSI device driver for Linux", Rik Faith (faith@cs.unc.edu)

 "Kernel Hacker's Guide", Michael K. Johnson (johnsonm@sunsite.unc.edu)

 "Adaptec 1520/1522 User's Guide", Adaptec Corporation.
 
 Michael K. Johnson (johnsonm@sunsite.unc.edu)

 Drew Eckhardt (drew@cs.colorado.edu)

 Eric Youngdale (eric@tantalus.nrl.navy.mil) 

 special thanks to Eric Youngdale for the free(!) supplying the
 documentation on the chip.

 **************************************************************************/

#include "aha152x.h"

#include <linux/sched.h>
#include <asm/io.h>
#include "../block/blk.h"
#include "scsi.h"
#include "hosts.h"
#include "constants.h"
#include <asm/system.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/ioport.h>

/* DEFINES */


/* If auto configuration is disabled, IRQ, SCSI_ID and RECONNECT have to
   be predefined */
#if !defined(AUTOCONF)
#if !defined(IRQ)
#error undefined IRQ; define AUTOCONF or IRQ
#endif
#if !defined(SCSI_ID)
#error undefined SCSI_ID; define AUTOCONF or SCSI_ID
#endif
#if !defined(RECONNECT)
#error undefined RECONNECT; define AUTOCONF or RECONNECT
#endif
#endif

/* I use this when I'm looking for weird bugs */
#define DEBUG_TIMING 

#if defined(DEBUG)

#undef  SKIP_PORTS              /* don't display ports */

#undef  DEBUG_QUEUE             /* debug queue() */
#undef  DEBUG_RESET             /* debug reset() */
#undef  DEBUG_INTR              /* debug intr() */
#undef  DEBUG_SELECTION         /* debug selection part in intr() */
#undef  DEBUG_MSGO              /* debug message out phase in intr() */
#undef  DEBUG_MSGI              /* debug message in phase in intr() */
#undef  DEBUG_STATUS            /* debug status phase in intr() */
#undef  DEBUG_CMD               /* debug command phase in intr() */
#undef  DEBUG_DATAI             /* debug data in phase in intr() */
#undef  DEBUG_DATAO             /* debug data out phase in intr() */
#undef  DEBUG_ABORT             /* debug abort() */
#undef  DEBUG_DONE              /* debug done() */
#undef  DEBUG_BIOSPARAM         /* debug biosparam() */

#undef  DEBUG_RACE              /* debug race conditions */
#undef  DEBUG_PHASES            /* debug phases (useful to trace) */
#undef  DEBUG_QUEUES            /* debug reselection */

/* recently used for debugging */
#if 0
#define DEBUG_PHASES
#define DEBUG_DATAI
#endif

#endif

#define DEBUG_RESET             /* resets should be rare */
#define DEBUG_ABORT             /* aborts too */

/* END OF DEFINES */

/* some additional "phases" for getphase() */
#define P_BUSFREE  1
#define P_PARITY   2

char *aha152x_id = "Adaptec 152x SCSI driver; $Revision: 0.101 $\n";

static int port_base      = 0;
static int this_host      = 0;
static int can_disconnect = 0;
static int commands       = 0;

/* set by aha152x_setup according to the command line */
static int setup_called    = 0;
static int setup_portbase  = 0;
static int setup_irq       = 0;
static int setup_scsiid    = 0;
static int setup_reconnect = 0;

static char *setup_str = (char *)NULL;

enum {
   not_issued   = 0x01,
   in_selection = 0x02,
   disconnected = 0x04,
   aborted      = 0x08,
   sent_ident   = 0x10,
   in_other     = 0x20,
};
 
/*
 * Command queues:
 * issue_SC        : commands that are queued to be issued 
 * current_SC      : command that's currently using the bus
 * disconnected_SC : commands that that have been disconnected 
 */
static Scsi_Cmnd            *issue_SC        = NULL;
static Scsi_Cmnd            *current_SC      = NULL;
static Scsi_Cmnd            *disconnected_SC = NULL;

static struct wait_queue    *abortion_complete;
static int                  abort_result;

void aha152x_intr( int irqno );
void aha152x_done( int error );
void aha152x_setup( char *str, int *ints );

static void aha152x_reset_ports(void);
static void aha152x_panic(char *msg);

static void disp_ports(void);
static void show_command(Scsi_Cmnd *ptr);
static void show_queues(void);
static void disp_enintr(void);

#if defined(DEBUG_RACE)
static void enter_driver(const char *);
static void leave_driver(const char *);
#endif

/* possible locations for the Adaptec BIOS */
static void *addresses[] =
{
  (void *) 0xdc000,   /* default first */
  (void *) 0xc8000,
  (void *) 0xcc000,
  (void *) 0xd0000,
  (void *) 0xd4000,
  (void *) 0xd8000,
  (void *) 0xe0000,
  (void *) 0xf0000,
};
#define ADDRESS_COUNT (sizeof( addresses ) / sizeof( unsigned ))

/* possible i/o adresses for the AIC-6260 */
static unsigned short ports[] =
{
  0x340,      /* default first */
  0x140
};
#define PORT_COUNT (sizeof( ports ) / sizeof( unsigned short ))

/* possible interrupt channels */
static unsigned short ints[] = { 9, 10, 11, 12 };

/* signatures for various AIC-6260 based controllers */
static struct signature {
  char *signature;
  int  sig_offset;
  int  sig_length;
} signatures[] =
{
  {
    "Adaptec AHA-1520 BIOS\r\n\0\
Version 1.4      \r\n\0\
Copyright 1990 Adaptec, Inc.\r\n\
All Rights Reserved\r\n \r\n \r\n", 0x102e, 101
  },                                                          /* Adaptec 152x */
  {
    "Adaptec ASW-B626 BIOS\r\n\0\
Version 1.0      \r\n\0\
Copyright 1990 Adaptec, Inc.\r\n\
All Rights Reserved\r\n\0 \r\n \r\n", 0x1029, 102
  },                                                   /* on-board controller */
  { "Adaptec BIOS: ASW-B626", 0x0F, 22},               /* on-board controller */
  { "Adaptec ASW-B626 S2 BIOS", 0x2e6c, 24},           /* on-board controller */
};
#define SIGNATURE_COUNT (sizeof( signatures ) / sizeof( struct signature ))


static void do_pause( unsigned amount ) /* Pause for amount*10 milliseconds */
{
   unsigned long the_time = jiffies + amount; /* 0.01 seconds per jiffy */

   while (jiffies < the_time)
     ;
}

/*
 *  queue services:
 */
static inline void append_SC( Scsi_Cmnd **SC, Scsi_Cmnd *new_SC)
{
  Scsi_Cmnd *end;

  new_SC->host_scribble = (unsigned char *) NULL;
  if(!*SC)
    *SC=new_SC;
  else
    {
      for( end=*SC;
           end->host_scribble;
           end = (Scsi_Cmnd *) end->host_scribble )
        ;
      end->host_scribble = (unsigned char *) new_SC;
    }
}

static inline Scsi_Cmnd *remove_first_SC( Scsi_Cmnd **SC )
{
  Scsi_Cmnd *ptr;

  ptr=*SC;
  if(ptr)
    *SC= (Scsi_Cmnd *) (*SC)->host_scribble;
  return ptr;
}

static inline Scsi_Cmnd *remove_SC( Scsi_Cmnd **SC, int target, int lun )
{
  Scsi_Cmnd *ptr, *prev;

  for( ptr=*SC, prev=NULL;
       ptr && ((ptr->target!=target) || (ptr->lun!=lun));
       prev = ptr, ptr = (Scsi_Cmnd *) ptr->host_scribble )
    ;

  if(ptr)
    if(prev)
      prev->host_scribble = ptr->host_scribble;
    else
      *SC= (Scsi_Cmnd *) ptr->host_scribble;
  return ptr;
}

/*
 * read inbound byte and wait for ACK to get low
 */
static void make_acklow(void)
{
  SETPORT( SXFRCTL0, CH1|SPIOEN );
  GETPORT(SCSIDAT);
  SETPORT( SXFRCTL0, CH1 );

  while( TESTHI( SCSISIG, ACKI ) )
    ;
}

/*
 * detect current phase more reliable:
 * phase is valid, when the target asserts REQ after we've deasserted ACK.
 *
 * return value is a valid phase or an error code.
 *
 * errorcodes:
 *   P_BUSFREE   BUS FREE phase detected
 *   P_PARITY    parity error in DATA phase
 */
static int getphase(void)
{
  int phase, sstat1;
  
  while( 1 )
    {
      do
        {
          while( !( ( sstat1 = GETPORT( SSTAT1 ) ) & (BUSFREE|SCSIRSTI|REQINIT ) ) )
            ;
          if( sstat1 & BUSFREE )
            return P_BUSFREE;
          if( sstat1 & SCSIRSTI )
            {
              /* IBM drive responds with RSTI to RSTO */
              printk("aha152x: RESET IN\n");
              SETPORT( SSTAT1, SCSIRSTI );
            }
        }
      while( TESTHI( SCSISIG, ACKI ) || TESTLO( SSTAT1, REQINIT ) );

      SETPORT( SSTAT1, CLRSCSIPERR );
  
      phase = GETPORT( SCSISIG ) & P_MASK ;

      if( TESTHI( SSTAT1, SCSIPERR ) )
        {
          if( (phase & (CDO|MSGO))==0 )                         /* DATA phase */
            return P_PARITY;

          make_acklow();
        }
      else
        return phase;
    }
}

/* called from init/main.c */
void aha152x_setup( char *str, int *ints)
{
  if(setup_called)
    panic("aha152x: aha152x_setup called twice.\n");

  setup_called=ints[0];
  setup_str=str;

  if(ints[0] != 4)
    return; 

  setup_portbase  = ints[1];
  setup_irq       = ints[2];
  setup_scsiid    = ints[3];
  setup_reconnect = ints[4];
}

/*
   Test, if port_base is valid.
 */
static int aha152x_porttest(int port_base)
{
  int i;

  if(check_region(port_base, TEST-SCSISEQ))
    return 0;

  SETPORT( DMACNTRL1, 0 );          /* reset stack pointer */
  for(i=0; i<16; i++)
    SETPORT( STACK, i );

  SETPORT( DMACNTRL1, 0 );          /* reset stack pointer */
  for(i=0; i<16 && GETPORT(STACK)==i; i++)
    ;

  return(i==16);
}

int aha152x_detect(int hostno)
{
  int                 i, j,  ok;
  aha152x_config      conf;
  struct sigaction    sa;
  int                 interrupt_level;
  
#if defined(DEBUG_RACE)
  enter_driver("detect");
#endif
  
  printk("aha152x: Probing: ");

  if(setup_called)
    {
      printk("processing commandline: ");
   
      if(setup_called!=4)
        {
          printk("\naha152x: %s\n", setup_str );
          printk("aha152x: usage: aha152x=<PORTBASE>,<IRQ>,<SCSI ID>,<RECONNECT>\n");
          panic("aha152x panics in line %d", __LINE__);
        }

      port_base       = setup_portbase;
      interrupt_level = setup_irq;
      this_host       = setup_scsiid;
      can_disconnect  = setup_reconnect;

      for( i=0; i<PORT_COUNT && (port_base != ports[i]); i++)
        ;

      if(i==PORT_COUNT)
        {
          printk("unknown portbase 0x%03x\n", port_base);
          panic("aha152x panics in line %d", __LINE__);
        }

      if(!aha152x_porttest(port_base))
        {
          printk("portbase 0x%03x fails probe\n", port_base);
          panic("aha152x panics in line %d", __LINE__);
        }

      i=0;
      while(ints[i] && (interrupt_level!=ints[i]))
        i++;
      if(!ints[i])
        {
          printk("illegal IRQ %d\n", interrupt_level);
          panic("aha152x panics in line %d", __LINE__);
        }

      if( (this_host < 0) || (this_host > 7) )
        {
          printk("illegal SCSI ID %d\n", this_host);
          panic("aha152x panics in line %d", __LINE__);
        }

      if( (can_disconnect < 0) || (can_disconnect > 1) )
        {
          printk("reconnect %d should be 0 or 1\n", can_disconnect);
          panic("aha152x panics in line %d", __LINE__);
        }
      printk("ok, ");
    }
  else
    {
#if !defined(SKIP_BIOSTEST)
      printk("BIOS test: ");
      ok=0;
      for( i=0; i < ADDRESS_COUNT && !ok; i++)
        for( j=0; (j < SIGNATURE_COUNT) && !ok; j++)
          ok=!memcmp((void *) addresses[i]+signatures[j].sig_offset,
                     (void *) signatures[j].signature,
                     (int) signatures[j].sig_length);

      if(!ok)
        {
#if defined(DEBUG_RACE)
          leave_driver("(1) detect");
#endif
          printk("failed\n");
          return 0;
        }
      printk("ok, ");
#endif /* !SKIP_BIOSTEST */
 
#if !defined(PORTBASE)
      printk("porttest: ");
      for( i=0; i<PORT_COUNT && !aha152x_porttest(ports[i]); i++)
        ;

      if(i==PORT_COUNT)
        {
          printk("failed\n");
#if defined(DEBUG_RACE)
          leave_driver("(2) detect");
#endif
          return 0;
        }
      else
        port_base=ports[i];
      printk("ok, ");
#else
      port_base=PORTBASE;
#endif /* !PORTBASE */

#if defined(AUTOCONF)

      conf.cf_port = (GETPORT(PORTA)<<8) + GETPORT(PORTB);

      interrupt_level = ints[conf.cf_irq];
      this_host       = conf.cf_id;
      can_disconnect  = conf.cf_tardisc;

      printk("auto configuration: ok, ");

#endif /* AUTOCONF */

#if defined(IRQ)
      interrupt_level = IRQ; 
#endif

#if defined(SCSI_ID)
      this_host = SCSI_ID;
#endif

#if defined(RECONNECT)
      can_disconnect=RECONNECT;
#endif
    }

  printk("detection complete\n");
 
  sa.sa_handler  = aha152x_intr;
  sa.sa_flags    = SA_INTERRUPT;
  sa.sa_mask     = 0;
  sa.sa_restorer = NULL;
  
  ok = irqaction( interrupt_level, &sa);
  
  if(ok<0)
    {
      if(ok == -EINVAL)
        {
           printk("aha152x: bad IRQ %d.\n", interrupt_level);
           printk("         Contact author.\n");
        }
      else
        if( ok == -EBUSY)
          printk( "aha152x: IRQ %d already in use. Configure another.\n",
                  interrupt_level);
        else
          {
            printk( "\naha152x: Unexpected error code on requesting IRQ %d.\n",
                    interrupt_level);
            printk("         Contact author.\n");
          }
      panic("aha152x: driver needs an IRQ.\n");
    }

  SETPORT( SCSIID, this_host << 4 );
  scsi_hosts[hostno].this_id=this_host;
  
  if(can_disconnect)
    scsi_hosts[hostno].can_queue=AHA152X_MAXQUEUE;

  /* RESET OUT */
  SETBITS(SCSISEQ, SCSIRSTO );
  do_pause(5);
  CLRBITS(SCSISEQ, SCSIRSTO );
  do_pause(10);

  aha152x_reset(NULL);

  printk("aha152x: vital data: PORTBASE=0x%03x, IRQ=%d, SCSI ID=%d, reconnect=%s, parity=enabled\n",
         port_base, interrupt_level, this_host, can_disconnect ? "enabled" : "disabled" );

  snarf_region(port_base, TEST-SCSISEQ);        /* Register */
  
  /* not expecting any interrupts */
  SETPORT(SIMODE0, 0);
  SETPORT(SIMODE1, 0);

#if defined(DEBUG_RACE)
  leave_driver("(3) detect");
#endif

  SETBITS( DMACNTRL0, INTEN);
  return 1;
}

/*
 *  return the name of the thing
 */
const char *aha152x_info(void)
{
#if defined(DEBUG_RACE)
  enter_driver("info");
  leave_driver("info");
#else
#if defined(DEBUG_INFO)
  printk("\naha152x: info()\n");
#endif
#endif
  return(aha152x_id);
}

/* 
 *  Queue a command and setup interrupts for a free bus.
 */
int aha152x_queue( Scsi_Cmnd * SCpnt, void (*done)(Scsi_Cmnd *))
{
#if defined(DEBUG_RACE)
  enter_driver("queue");
#else
#if defined(DEBUG_QUEUE)
  printk("aha152x: queue(), ");
#endif
#endif


#if defined(DEBUG_QUEUE)
  printk( "SCpnt (target = %d lun = %d cmnd = 0x%02x pieces = %d size = %u), ",
          SCpnt->target,
          SCpnt->lun,
          *(unsigned char *)SCpnt->cmnd,
          SCpnt->use_sg,
          SCpnt->request_bufflen );
  disp_ports();
#endif

  SCpnt->scsi_done =       done;

  /* setup scratch area
     SCp.ptr              : buffer pointer
     SCp.this_residual    : buffer length
     SCp.buffer           : next buffer
     SCp.buffers_residual : left buffers in list
     SCp.phase            : current state of the command */
  SCpnt->SCp.phase = not_issued;
  if (SCpnt->use_sg)
    {
      SCpnt->SCp.buffer           = (struct scatterlist *)SCpnt->request_buffer;
      SCpnt->SCp.ptr              = SCpnt->SCp.buffer->address;
      SCpnt->SCp.this_residual    = SCpnt->SCp.buffer->length;
      SCpnt->SCp.buffers_residual = SCpnt->use_sg - 1;
    }
  else
    {
      SCpnt->SCp.ptr              = (char *)SCpnt->request_buffer;
      SCpnt->SCp.this_residual    = SCpnt->request_bufflen;
      SCpnt->SCp.buffer           = NULL;
      SCpnt->SCp.buffers_residual = 0;
    }
          
  SCpnt->SCp.Status              = CHECK_CONDITION;
  SCpnt->SCp.Message             = 0;
  SCpnt->SCp.have_data_in        = 0;
  SCpnt->SCp.sent_command        = 0;

  /* Turn led on, when this is the first command. */
  cli();
  commands++;
  if(commands==1)
    SETPORT( PORTA, 1 );

#if defined(DEBUG_QUEUES)
  printk("i+ (%d), ", commands );
#endif
  append_SC( &issue_SC, SCpnt);
  
  /* Enable bus free interrupt, when we aren't currently on the bus */
  if(!current_SC)
    {
      SETPORT(SIMODE0, disconnected_SC ? ENSELDI : 0 );
      SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);
    }
  sti();

  return 0;
}

/*
 *  We only support command in interrupt-driven fashion
 */
int aha152x_command( Scsi_Cmnd *SCpnt )
{
  printk( "aha152x: interrupt driven driver; use aha152x_queue()\n" );
  return -1;
}

/*
 *  Abort a queued command
 *  (commands that are on the bus can't be aborted easily)
 */
int aha152x_abort( Scsi_Cmnd *SCpnt, int code )
{
  Scsi_Cmnd *ptr, *prev;

  cli();

#if defined(DEBUG_ABORT)
  printk("aha152x: abort(), SCpnt=0x%08x, ", (unsigned long) SCpnt );
#endif

  show_queues();

  /* look for command in issue queue */
  for( ptr=issue_SC, prev=NULL;
       ptr && ptr!=SCpnt;
       prev=ptr, ptr=(Scsi_Cmnd *) ptr->host_scribble)
    ;

  if(ptr)
    {
      /* dequeue */
      if(prev)
        prev->host_scribble = ptr->host_scribble;
      else
        issue_SC = (Scsi_Cmnd *) ptr->host_scribble;
      sti();

      ptr->host_scribble = NULL;
      ptr->result = (code ? code : DID_ABORT ) << 16;
      ptr->done(ptr);
      return 0;
    }

  /* Fail abortion, if we're on the bus */
  if (current_SC)
    {
       sti();
       return -1;
    }

  /* look for command in disconnected queue */
  for( ptr=disconnected_SC, prev=NULL;
       ptr && ptr!=SCpnt;
       prev=ptr, ptr=(Scsi_Cmnd *) ptr->host_scribble)
    ;

  if(ptr && TESTLO(SSTAT1, BUSFREE) )
    printk("bus busy but no current command, ");

  if(ptr && TESTHI(SSTAT1, BUSFREE) )
    {
      /* dequeue */
      if(prev)
        prev->host_scribble = ptr->host_scribble;
      else
        issue_SC = (Scsi_Cmnd *) ptr->host_scribble;

      /* set command current and initiate selection,
         let the interrupt routine take care of the abortion */
      current_SC     = ptr;
      ptr->SCp.phase = in_selection|aborted;
      SETPORT( SCSIID, (this_host << OID_) | current_SC->target );

      /* enable interrupts for SELECTION OUT DONE and SELECTION TIME OUT */
      SETPORT( SIMODE0, ENSELDO | (disconnected_SC ? ENSELDI : 0) );
      SETPORT( SIMODE1, ENSELTIMO );

      /* Enable SELECTION OUT sequence */
      SETBITS(SCSISEQ, ENSELO | ENAUTOATNO );

      SETBITS( DMACNTRL0, INTEN );
      abort_result=0;
      sti();

      /* sleep until the abortion is complete */
      sleep_on( &abortion_complete );
      return abort_result;
    }
  else
    printk("aha152x: bus busy but no current command\n");

  /* command wasn't found */
  sti();
  return 0;
}

/*
 *  Restore default values to the AIC-6260 registers and reset the fifos
 */
static void aha152x_reset_ports(void)
{
  /* disable interrupts */
  SETPORT(DMACNTRL0, RSTFIFO);

  SETPORT(SCSISEQ, 0);

  SETPORT(SXFRCTL1, 0);
  SETPORT( SCSISIG, 0);
  SETPORT(SCSIRATE, 0);

  /* clear all interrupt conditions */
  SETPORT(SSTAT0, 0x7f);
  SETPORT(SSTAT1, 0xef);

  SETPORT(SSTAT4, SYNCERR|FWERR|FRERR);

  SETPORT(DMACNTRL0, 0);
  SETPORT(DMACNTRL1, 0);

  SETPORT(BRSTCNTRL, 0xf1);

  /* clear SCSI fifo and transfer count */
  SETPORT(SXFRCTL0, CH1|CLRCH1|CLRSTCNT);
  SETPORT(SXFRCTL0, CH1);

  /* enable interrupts */
  SETPORT(SIMODE0, disconnected_SC ? ENSELDI : 0 );
  SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);
}

/*
 *  Reset registers, reset a hanging bus and
 *  kill active and disconnected commands
 */
int aha152x_reset(Scsi_Cmnd * __unused)
{
  Scsi_Cmnd *ptr;

  aha152x_reset_ports();

  /* Reset, if bus hangs */
  if( TESTLO( SSTAT1, BUSFREE ) )
    {
       CLRBITS( DMACNTRL0, INTEN );

#if defined( DEBUG_RESET )
       printk("aha152x: reset(), bus not free: SCSI RESET OUT\n");
#endif

       show_queues();

       if(current_SC)
         {
           current_SC->host_scribble = NULL;
           current_SC->result = DID_RESET << 16;
           current_SC->done(current_SC);
           current_SC=NULL;
         }

       while(disconnected_SC)
         {
           ptr = disconnected_SC;
           disconnected_SC = (Scsi_Cmnd *) ptr->host_scribble;
           ptr->host_scribble = NULL;
           ptr->result = DID_RESET << 16;
           ptr->done(ptr);
         }

       /* RESET OUT */
       SETPORT(SCSISEQ, SCSIRSTO);
       do_pause(5);
       SETPORT(SCSISEQ, 0);
       do_pause(10);

       SETPORT(SIMODE0, 0 );
       SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);

       SETPORT( DMACNTRL0, INTEN );
    }

  return 0;
}

/*
 * Return the "logical geometry"
 */
int aha152x_biosparam( int size, int dev, int *info_array )
{
#if defined(DEBUG_RACE)
  enter_driver("biosparam");
#else
#if defined(DEBUG_BIOSPARAM)
  printk("\naha152x: biosparam(), ");
#endif
#endif

#if defined(DEBUG_BIOSPARAM)
  printk("dev=%x, size=%d, ", dev, size);
#endif
  
/* I took this from other SCSI drivers, since it provides
   the correct data for my devices. */
  info_array[0]=64;
  info_array[1]=32;
  info_array[2]=size>>11;

#if defined(DEBUG_BIOSPARAM)
  printk("bios geometry: head=%d, sec=%d, cyl=%d\n",
         info_array[0], info_array[1], info_array[2]);
  printk("WARNING: check, if the bios geometry is correct.\n");
#endif

#if defined(DEBUG_RACE)
  leave_driver("biosparam");
#endif
  return 0;
}

/*
 *  Internal done function
 */
void aha152x_done( int error )
{
  Scsi_Cmnd *done_SC;

#if defined(DEBUG_DONE)
  printk("\naha152x: done(), ");
  disp_ports();
#endif

  if (current_SC)
    {
#if defined(DEBUG_DONE)
      printk("done(%x), ", error);
#endif

      cli();

      done_SC = current_SC;
      current_SC = NULL;

      /* turn led off, when no commands are in the driver */
      commands--;
      if(!commands)
        SETPORT( PORTA, 0 );                                  /* turn led off */

#if defined(DEBUG_QUEUES)
      printk("ok (%d), ", commands);
#endif
      sti();

      SETPORT(SIMODE0, disconnected_SC ? ENSELDI : 0 );
      SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);

#if defined(DEBUG_PHASES)
      printk("BUS FREE loop, ");
#endif
      while( TESTLO( SSTAT1, BUSFREE ) )
        ;
#if defined(DEBUG_PHASES)
      printk("BUS FREE\n");
#endif

      done_SC->result = error;
      if(done_SC->scsi_done)
        {
#if defined(DEBUG_DONE)
          printk("calling scsi_done, ");
#endif
          done_SC->scsi_done( done_SC );
#if defined(DEBUG_DONE)
          printk("done returned, ");
#endif
        }
      else
        panic( "aha152x: current_SC->scsi_done() == NULL" );
    }
  else
    aha152x_panic( "done() called outside of command" );
}

/*
 * Interrupts handler (main routine of the driver)
 */
void aha152x_intr( int irqno )
{
  int done=0, phase;

#if defined(DEBUG_RACE)
  enter_driver("intr");
#else
#if defined(DEBUG_INTR)
  printk("\naha152x: intr(), ");
#endif
#endif

  /* no more interrupts from the controller, while we busy.
     INTEN has to be restored, when we're ready to leave
     intr(). To avoid race conditions we have to return
     immediately afterwards. */
  CLRBITS( DMACNTRL0, INTEN);
  sti();

  /* disconnected target is trying to reconnect.
     Only possible, if we have disconnected nexuses and
     nothing is occuping the bus.
  */
  if( TESTHI( SSTAT0, SELDI ) &&
      disconnected_SC &&
      ( !current_SC || ( current_SC->SCp.phase & in_selection ) )
    )
    {
      int identify_msg, target, i;

      /* Avoid conflicts when a target reconnects
         while we are trying to connect to another. */
      if(current_SC)
        {
#if defined(DEBUG_QUEUES)
          printk("i+, ");
#endif
          cli();
          append_SC( &issue_SC, current_SC);
          current_SC=NULL;
          sti();
        }

      /* disable sequences */
      SETPORT( SCSISEQ, 0 );
      SETPORT( SSTAT0, CLRSELDI );
      SETPORT( SSTAT1, CLRBUSFREE );

#if defined(DEBUG_QUEUES) || defined(DEBUG_PHASES)
      printk("reselected, ");
#endif

      i = GETPORT(SELID) & ~(1 << this_host);
      target=0;
      if(i)
        for( ; (i & 1)==0; target++, i>>=1)
          ;
      else
        aha152x_panic("reconnecting target unknown");

#if defined(DEBUG_QUEUES)
      printk("SELID=%02x, target=%d, ", GETPORT(SELID), target );
#endif
      SETPORT( SCSIID, (this_host << OID_) | target );
      SETPORT( SCSISEQ, ENRESELI );

      if(TESTLO( SSTAT0, SELDI ))
        aha152x_panic("RESELI failed");

      SETPORT( SCSISIG, P_MSGI );

      /* Get identify message */
      if((i=getphase())!=P_MSGI)
        {
          printk("target doesn't enter MSGI to identify (phase=%02x)\n", i);
          aha152x_panic("unknown lun");
        }
      SETPORT( SCSISEQ, 0 );

      SETPORT( SXFRCTL0, CH1);

      identify_msg = GETPORT(SCSIBUS);

      if(!(identify_msg & IDENTIFY_BASE))
        {
          printk("target=%d, inbound message (%02x) != IDENTIFY\n",
                 target, identify_msg);
          aha152x_panic("unknown lun");
        }

      make_acklow();
      getphase();

#if defined(DEBUG_QUEUES)
      printk("identify=%02x, lun=%d, ", identify_msg, identify_msg & 0x3f );
#endif

      cli();
#if defined(DEBUG_QUEUES)
      printk("d-, ");
#endif
      current_SC = remove_SC( &disconnected_SC,
                              target,
                              identify_msg & 0x3f );

      if(!current_SC)
        {
          printk("lun=%d, ", identify_msg & 0x3f );
          aha152x_panic("no disconnected command for that lun");
        }

      current_SC->SCp.phase &= ~disconnected;
      sti();

      SETPORT( SIMODE0, 0 );
      SETPORT( SIMODE1, ENPHASEMIS );
#if defined(DEBUG_RACE)
      leave_driver("(reselected) intr");
#endif
      SETBITS( DMACNTRL0, INTEN);
      return;
    }
  
  /* Check, if we aren't busy with a command */
  if(!current_SC)
    {
      /* bus is free to issue a queued command */
      if(TESTHI( SSTAT1, BUSFREE) && issue_SC)
        {
          cli();
#if defined(DEBUG_QUEUES)
          printk("i-, ");
#endif
          current_SC = remove_first_SC( &issue_SC );
          sti();

#if defined(DEBUG_INTR) || defined(DEBUG_SELECTION) || defined(DEBUG_PHASES)
          printk("issueing command, ");
#endif
          current_SC->SCp.phase = in_selection;

  #if defined(DEBUG_INTR) || defined(DEBUG_SELECTION) || defined(DEBUG_PHASES)
          printk("selecting %d, ", current_SC->target); 
  #endif
          SETPORT( SCSIID, (this_host << OID_) | current_SC->target );

          /* Enable interrupts for SELECTION OUT DONE and SELECTION OUT INITIATED */
          SETPORT( SXFRCTL1, ENSPCHK|ENSTIMER);

          /* enable interrupts for SELECTION OUT DONE and SELECTION TIME OUT */
          SETPORT( SIMODE0, ENSELDO | (disconnected_SC ? ENSELDI : 0) );
          SETPORT( SIMODE1, ENSELTIMO );

          /* Enable SELECTION OUT sequence */
          SETBITS(SCSISEQ, ENSELO | ENAUTOATNO );
        
  #if defined(DEBUG_RACE)
          leave_driver("(selecting) intr");
  #endif
          SETBITS( DMACNTRL0, INTEN );
          return;
        }

      /* No command we are busy with and no new to issue */
      printk("aha152x: ignoring spurious interrupt, nothing to do\n");
      return;
    }

  /* the bus is busy with something */

#if defined(DEBUG_INTR)
  disp_ports();
#endif

  /* we are waiting for the result of a selection attempt */
  if(current_SC->SCp.phase & in_selection)
    {
      if( TESTLO( SSTAT1, SELTO ) )
        /* no timeout */
        if( TESTHI( SSTAT0, SELDO ) )
          {
            /* clear BUS FREE interrupt */
            SETPORT( SSTAT1, CLRBUSFREE);

            /* Disable SELECTION OUT sequence */
            CLRBITS(SCSISEQ, ENSELO|ENAUTOATNO );

            /* Disable SELECTION OUT DONE interrupt */
            CLRBITS(SIMODE0, ENSELDO);
            CLRBITS(SIMODE1, ENSELTIMO);

            if( TESTLO(SSTAT0, SELDO) )
              {
                printk("aha152x: passing bus free condition\n");

#if defined(DEBUG_RACE)
                leave_driver("(passing bus free) intr");
#endif
                SETBITS( DMACNTRL0, INTEN);

                if(current_SC->SCp.phase & aborted)
                  {
                    abort_result=1;
                    wake_up( &abortion_complete );
                  }

                aha152x_done( DID_NO_CONNECT << 16 );
                return;
              }
#if defined(DEBUG_SELECTION) || defined(DEBUG_PHASES)
            printk("SELDO (SELID=%x), ", GETPORT(SELID));
#endif

            /* selection was done */
            SETPORT( SSTAT0, CLRSELDO );

#if defined(DEBUG_ABORT)
            if(current_SC->SCp.phase & aborted)
              printk("(ABORT) target selected, ");
#endif

            current_SC->SCp.phase &= ~in_selection;
            current_SC->SCp.phase |= in_other;

#if defined(DEBUG_RACE)
            leave_driver("(SELDO) intr");
#endif

            SETPORT( SCSISIG, P_MSGO );

            SETPORT( SIMODE0, 0 );
            SETPORT( SIMODE1, ENREQINIT );
            SETBITS( DMACNTRL0, INTEN);
            return;
          }
        else
          aha152x_panic("neither timeout nor selection\007");
      else
        {
#if defined(DEBUG_SELECTION) || defined(DEBUG_PHASES)
          printk("SELTO, ");
#endif
	  /* end selection attempt */
          CLRBITS(SCSISEQ, ENSELO|ENAUTOATNO );

          /* timeout */
          SETPORT( SSTAT1, CLRSELTIMO );

          SETPORT(SIMODE0, disconnected_SC ? ENSELDI : 0 );
          SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);
          SETBITS( DMACNTRL0, INTEN );
#if defined(DEBUG_RACE)
          leave_driver("(SELTO) intr");
#endif

          if(current_SC->SCp.phase & aborted)
            {
#if defined(DEBUG_ABORT)
              printk("(ABORT) selection timeout, ");
#endif
              abort_result=1;
              wake_up( &abortion_complete );
            }

          if( TESTLO( SSTAT0, SELINGO ) )
            /* ARBITRATION not won */
            aha152x_done( DID_BUS_BUSY << 16 );
          else
            /* ARBITRATION won, but SELECTION failed */
            aha152x_done( DID_NO_CONNECT << 16 );
          return;
        }
    }

  /* enable interrupt, when target leaves current phase */
  phase = getphase();
  if(!(phase & ~P_MASK))                                      /* "real" phase */
    SETPORT(SCSISIG, phase);
  SETPORT(SSTAT1, CLRPHASECHG);
  current_SC->SCp.phase =
    (current_SC->SCp.phase & ~((P_MASK|1)<<16)) | (phase << 16 );

  /* information transfer phase */
  switch( phase )
    {
    case P_MSGO:                                               /* MESSAGE OUT */
      {
        unsigned char message;

#if defined(DEBUG_INTR) || defined(DEBUG_MSGO) || defined(DEBUG_PHASES)
        printk("MESSAGE OUT, ");
#endif

        if( current_SC->SCp.phase & aborted )
          {
#if defined(DEBUG_MSGO) || defined(DEBUG_ABORT)
            printk("ABORT, ");
#endif
            message=ABORT;
          }
        else
          /* If we didn't identify yet, do it. Otherwise there's nothing to do,
             but reject (probably we got an message before, that we have to
             reject (SDTR, WDTR, etc.) */
          if( !(current_SC->SCp.phase & sent_ident))
            {
              message=IDENTIFY(can_disconnect,current_SC->lun);
#if defined(DEBUG_MSGO)
              printk("IDENTIFY (reconnect=%s;lun=%d), ", 
                      can_disconnect ? "enabled" : "disabled", current_SC->lun);
#endif
            }
          else
            {
              message=MESSAGE_REJECT;
#if defined(DEBUG_MSGO)
              printk("REJECT, ");
#endif
            }
          
        CLRBITS( SXFRCTL0, ENDMA);

        SETPORT( SIMODE0, 0 );
        SETPORT( SIMODE1, ENPHASEMIS|ENREQINIT );

        /* wait for data latch to become ready or a phase change */
        while( TESTLO( DMASTAT, INTSTAT ) )
          ;

        if( TESTHI( SSTAT1, PHASEMIS ) )
          aha152x_panic("unable to send message");

        /* Leave MESSAGE OUT after transfer */
        SETPORT( SSTAT1, CLRATNO);

        SETPORT( SCSIDAT, message );

        make_acklow();
        getphase();

        if(message==IDENTIFY(can_disconnect,current_SC->lun))
          current_SC->SCp.phase |= sent_ident;

        if(message==ABORT)
          {
            /* revive abort(); abort() enables interrupts */
            abort_result=0;
            wake_up( &abortion_complete );

            current_SC->SCp.phase = (current_SC->SCp.phase & ~(P_MASK<<16));

            /* exit */
            SETBITS( DMACNTRL0, INTEN );
#if defined(DEBUG_RACE)
            leave_driver("(ABORT) intr");
#endif
            aha152x_done(DID_ABORT<<16);
            return;
          }
      }
      break;

    case P_CMD:                                          /* COMMAND phase */
#if defined(DEBUG_INTR) || defined(DEBUG_CMD) || defined(DEBUG_PHASES)
      printk("COMMAND, ");
#endif
      if( !(current_SC->SCp.sent_command) )
        {
          if(GETPORT(FIFOSTAT) || GETPORT(SSTAT2) & (SFULL|SFCNT))
            printk("aha152x: P_CMD: %d(%d) bytes left in FIFO, resetting\n",
                   GETPORT(FIFOSTAT), GETPORT(SSTAT2) & (SFULL|SFCNT));

          /* reset fifo and enable writes */
          SETPORT(DMACNTRL0, WRITE_READ|RSTFIFO);
          SETPORT(DMACNTRL0, ENDMA|WRITE_READ);

          /* clear transfer count and scsi fifo */
          SETPORT(SXFRCTL0, CH1|CLRSTCNT|CLRCH1 );
          SETPORT(SXFRCTL0, SCSIEN|DMAEN|CH1);
  
          /* missing phase raises INTSTAT */
          SETPORT( SIMODE0, 0 );
          SETPORT( SIMODE1, ENPHASEMIS );
  
#if defined(DEBUG_CMD)
          printk("waiting, ");
#endif
          /* wait for FIFO to get empty */
          while( TESTLO ( DMASTAT, DFIFOEMP|INTSTAT ) )
            ;
  
          if( TESTHI( SSTAT1, PHASEMIS ) )
            aha152x_panic("target left COMMAND phase");

#if defined(DEBUG_CMD)
          printk("DFIFOEMP, outsw (%d words), ",
                 COMMAND_SIZE(current_SC->cmnd[0])>>1);
          disp_ports();
#endif
  
          outsw( DATAPORT,
                 &current_SC->cmnd,
                 COMMAND_SIZE(current_SC->cmnd[0])>>1 );

#if defined(DEBUG_CMD)
          printk("FCNT=%d, STCNT=%d, ", GETPORT(FIFOSTAT), GETSTCNT() );
          disp_ports();
#endif

          /* wait for SCSI FIFO to get empty.
             very important to send complete commands. */
          while( TESTLO ( SSTAT2, SEMPTY ) )
            ;

          CLRBITS(SXFRCTL0, SCSIEN|DMAEN);
          /* transfer can be considered ended, when SCSIEN reads back zero */
          while( TESTHI( SXFRCTL0, SCSIEN ) )
            ;

          CLRBITS(DMACNTRL0, ENDMA);

#if defined(DEBUG_CMD) || defined(DEBUG_INTR)
          printk("sent %d/%d command bytes, ", GETSTCNT(),
                 COMMAND_SIZE(current_SC->cmnd[0]));
#endif

        }
      else
        aha152x_panic("Nothing to sent while in COMMAND OUT");
      break;

    case P_MSGI:                                          /* MESSAGE IN phase */
#if defined(DEBUG_INTR) || defined(DEBUG_MSGI) || defined(DEBUG_PHASES)
      printk("MESSAGE IN, ");
#endif
      SETPORT( SXFRCTL0, CH1);

      SETPORT( SIMODE0, 0);
      SETPORT( SIMODE1, ENBUSFREE);
  
      while( phase == P_MSGI ) 
        {
          current_SC->SCp.Message = GETPORT( SCSIBUS );
          switch(current_SC->SCp.Message)
            {
            case DISCONNECT:
#if defined(DEBUG_MSGI) || defined(DEBUG_PHASES)
              printk("target disconnected, ");
#endif
              current_SC->SCp.Message = 0;
              current_SC->SCp.phase   |= disconnected;
              if(!can_disconnect)
                aha152x_panic("target was not allowed to disconnect");
              break;
        
            case COMMAND_COMPLETE:
#if defined(DEBUG_MSGI) || defined(DEBUG_PHASES)
              printk("inbound message ( COMMAND COMPLETE ), ");
#endif
              done++;
              break;

            case MESSAGE_REJECT:
#if defined(DEBUG_MSGI) || defined(DEBUG_TIMING)
              printk("inbound message ( MESSAGE REJECT ), ");
#endif
              break;

            case SAVE_POINTERS:
#if defined(DEBUG_MSGI)
              printk("inbound message ( SAVE DATA POINTERS ), ");
#endif
              break;

            case EXTENDED_MESSAGE:
              { 
                int           i, code;

#if defined(DEBUG_MSGI)
                printk("inbound message ( EXTENDED MESSAGE ), ");
#endif
                make_acklow();
                if(getphase()!=P_MSGI)
                  break;
  
                i=GETPORT(SCSIBUS);

#if defined(DEBUG_MSGI)
                printk("length (%d), ", i);
#endif

#if defined(DEBUG_MSGI)
                printk("code ( ");
#endif

                make_acklow();
                if(getphase()!=P_MSGI)
                  break;

                code = GETPORT(SCSIBUS);

                switch( code )
                  {
                  case 0x00:
#if defined(DEBUG_MSGI)
                    printk("MODIFY DATA POINTER ");
#endif
                    SETPORT(SCSISIG, P_MSGI|ATNO);
                    break;
                  case 0x01:
#if defined(DEBUG_MSGI)
                    printk("SYNCHRONOUS DATA TRANSFER REQUEST ");
#endif
                    SETPORT(SCSISIG, P_MSGI|ATNO);
                    break;
                  case 0x02:
#if defined(DEBUG_MSGI)
                    printk("EXTENDED IDENTIFY ");
#endif
                    break;
                  case 0x03:
#if defined(DEBUG_MSGI)
                    printk("WIDE DATA TRANSFER REQUEST ");
#endif
                    SETPORT(SCSISIG, P_MSGI|ATNO);
                    break;
                  default:
#if defined(DEBUG_MSGI)
                    if( code & 0x80 )
                      printk("reserved (%d) ", code );
                    else
                      printk("vendor specific (%d) ", code);
#endif
                    SETPORT(SCSISIG, P_MSGI|ATNO);
                    break;
                  }
#if defined(DEBUG_MSGI)
                printk(" ), data ( ");
#endif
                while( --i && (make_acklow(), getphase()==P_MSGI))
                  {
#if defined(DEBUG_MSGI)
                    printk("%x ", GETPORT(SCSIBUS) );
#else
                    GETPORT(SCSIBUS);
#endif
                  }
#if defined(DEBUG_MSGI)
                printk(" ), ");
#endif
                /* We reject all extended messages. To do this
                   we just enter MSGO by asserting ATN. Since
                   we have already identified a REJECT message
                   will be sent. */
                SETPORT(SCSISIG, P_MSGI|ATNO);
              }
              break;
       
            default:
              printk("unsupported inbound message %x, ", current_SC->SCp.Message);
              break;

            }

          make_acklow();
          phase=getphase();
        } 

      /* clear SCSI fifo on BUSFREE */
      if(phase==P_BUSFREE)
        SETPORT(SXFRCTL0, CH1|CLRCH1);

      if(current_SC->SCp.phase & disconnected)
        {
          cli();
#if defined(DEBUG_QUEUES)
          printk("d+, ");
#endif
          append_SC( &disconnected_SC, current_SC);
          current_SC = NULL;
          sti();

          SETBITS( SCSISEQ, ENRESELI );

          SETPORT(SIMODE0, disconnected_SC ? ENSELDI : 0 );
          SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);

          SETBITS( DMACNTRL0, INTEN );
          return;
        }
      break;

    case P_STATUS:                                         /* STATUS IN phase */
#if defined(DEBUG_STATUS) || defined(DEBUG_INTR) || defined(DEBUG_PHASES)
      printk("STATUS, ");
#endif
      SETPORT( SXFRCTL0, CH1);

      SETPORT( SIMODE0, 0 );
      SETPORT( SIMODE1, ENREQINIT );

      if( TESTHI( SSTAT1, PHASEMIS ) )
	printk("aha152x: passing STATUS phase");
	
      current_SC->SCp.Status = GETPORT( SCSIBUS );
      make_acklow();
      getphase();

#if defined(DEBUG_STATUS)
      printk("inbound status ");
      print_status( current_SC->SCp.Status );
      printk(", ");
#endif
      break;

    case P_DATAI:                                            /* DATA IN phase */
      {
        int fifodata, data_count, done;

#if defined(DEBUG_DATAI) || defined(DEBUG_INTR) || defined(DEBUG_PHASES)
        printk("DATA IN, ");
#endif

        if(GETPORT(FIFOSTAT) || GETPORT(SSTAT2) & (SFULL|SFCNT))
          printk("aha152x: P_DATAI: %d(%d) bytes left in FIFO, resetting\n",
                 GETPORT(FIFOSTAT), GETPORT(SSTAT2) & (SFULL|SFCNT));

        /* reset host fifo */
        SETPORT(DMACNTRL0, RSTFIFO);
        SETPORT(DMACNTRL0, RSTFIFO|ENDMA);

        SETPORT(SXFRCTL0, CH1|SCSIEN|DMAEN );

        SETPORT( SIMODE0, 0 );
        SETPORT( SIMODE1, ENPHASEMIS|ENBUSFREE );

        /* done is set when the FIFO is empty after the target left DATA IN */
        done=0;
      
        /* while the target stays in DATA to transfer data */
        while ( !done ) 
          {
#if defined(DEBUG_DATAI)
            printk("expecting data, ");
#endif
            /* wait for PHASEMIS or full FIFO */
            while( TESTLO ( DMASTAT, DFIFOFULL|INTSTAT ) )
              ;

            if( TESTHI( DMASTAT, DFIFOFULL ) )
              fifodata=132;
            else
              {
                /* wait for SCSI fifo to get empty */
                while( TESTLO( SSTAT2, SEMPTY ) )
                  ;

                /* rest of data in FIFO */
                fifodata=GETPORT(FIFOSTAT);
#if defined(DEBUG_DATAI)
                printk("last transfer, ");
#endif
                done=1;
              }
  
#if defined(DEBUG_DATAI)
            printk("fifodata=%d, ", fifodata);
#endif

            while( fifodata && current_SC->SCp.this_residual )
              {
                data_count=fifodata;
  
                /* limit data transfer to size of first sg buffer */
                if (data_count > current_SC->SCp.this_residual)
                  data_count = current_SC->SCp.this_residual;
  
                fifodata -= data_count;

#if defined(DEBUG_DATAI)
                printk("data_count=%d, ", data_count);
#endif
  
                if(data_count == 1)
                  {
                    /* get a single byte in byte mode */
                    SETBITS(DMACNTRL0, _8BIT );
                    *current_SC->SCp.ptr++ = GETPORT( DATAPORT );
                    current_SC->SCp.this_residual--;
                  }
                else
                  {
                    CLRBITS(DMACNTRL0, _8BIT );
                    data_count >>= 1; /* Number of words */
                    insw( DATAPORT, current_SC->SCp.ptr, data_count );
#if defined(DEBUG_DATAI)
/* show what comes with the last transfer */
                    if(done)
                      {
                        int           i;
                        unsigned char *data;

                        printk("data on last transfer (%d bytes: ",
                               2*data_count);
                        data = (unsigned char *) current_SC->SCp.ptr;
                        for( i=0; i<2*data_count; i++)
                          printk("%2x ", *data++);
                        printk("), ");
                      }
#endif
                    current_SC->SCp.ptr           += 2 * data_count;
                    current_SC->SCp.this_residual -= 2 * data_count;
                  }
              
                /* if this buffer is full and there are more buffers left */
                if (!current_SC->SCp.this_residual &&
                     current_SC->SCp.buffers_residual)
                  {
                    /* advance to next buffer */
                    current_SC->SCp.buffers_residual--;
                    current_SC->SCp.buffer++;
                    current_SC->SCp.ptr =
                      current_SC->SCp.buffer->address;
                    current_SC->SCp.this_residual =
                      current_SC->SCp.buffer->length;
                  } 
              }
 
            /* rare (but possible) status bytes (probably also DISCONNECT 
               messages) get transfered in the data phase, so I assume 1
               additional byte is ok */
            if(fifodata>1)
              {
                printk("aha152x: more data than expected (%d bytes)\n",
                       GETPORT(FIFOSTAT));
              }

#if defined(DEBUG_DATAI)
            if(!fifodata)
              printk("fifo empty, ");
            else
              printk("something left in fifo, ");
#endif
          }

#if defined(DEBUG_DATAI)
        if(current_SC->SCp.buffers_residual || current_SC->SCp.this_residual)
          printk("left buffers (buffers=%d, bytes=%d), ",
                 current_SC->SCp.buffers_residual, 
                 current_SC->SCp.this_residual);
#endif
        /* transfer can be considered ended, when SCSIEN reads back zero */
        CLRBITS(SXFRCTL0, SCSIEN|DMAEN);
        while( TESTHI( SXFRCTL0, SCSIEN ) )
          ;
        CLRBITS(DMACNTRL0, ENDMA );

#if defined(DEBUG_DATAI) || defined(DEBUG_INTR)
        printk("got %d bytes, ", GETSTCNT());
#endif

        current_SC->SCp.have_data_in++;
      }
      break;

    case P_DATAO:                                           /* DATA OUT phase */
      {
        int data_count;

#if defined(DEBUG_DATAO) || defined(DEBUG_INTR) || defined(DEBUG_PHASES)
        printk("DATA OUT, ");
#endif
#if defined(DEBUG_DATAO)
        printk("got data to send (bytes=%d, buffers=%d), ",
               current_SC->SCp.this_residual,
               current_SC->SCp.buffers_residual );
#endif

        if(GETPORT(FIFOSTAT) || GETPORT(SSTAT2) & (SFULL|SFCNT) )
          {
            printk("%d(%d) left in FIFO, ", GETPORT(FIFOSTAT), GETPORT(SSTAT2) & (SFULL|SFCNT) );
            aha152x_panic("FIFO should be empty");
          }

        SETPORT(DMACNTRL0, WRITE_READ|RSTFIFO);
        SETPORT(DMACNTRL0, ENDMA|WRITE_READ);

        SETPORT(SXFRCTL0, CH1|CLRSTCNT|CLRCH1 );
        SETPORT(SXFRCTL0, SCSIEN|DMAEN|CH1);
 
        SETPORT( SIMODE0, 0 );
        SETPORT( SIMODE1, ENPHASEMIS );

        /* while current buffer is not empty or
           there are more buffers to transfer */
        while( TESTLO( SSTAT1, PHASEMIS ) &&
                 (current_SC->SCp.this_residual ||
                  current_SC->SCp.buffers_residual) )
          {
#if defined(DEBUG_DATAO)
            printk("sending data (left: bytes=%d, buffers=%d), waiting, ",
                    current_SC->SCp.this_residual,
                    current_SC->SCp.buffers_residual);
#endif
            /* transfer rest of buffer, but max. 128 byte */
            data_count = current_SC->SCp.this_residual > 128 ?
                         128 : current_SC->SCp.this_residual ;

#if defined(DEBUG_DATAO)
            printk("data_count=%d, ", data_count);
#endif
  
            if(data_count == 1)
              {
                /* put a single byte in byte mode */
                SETBITS(DMACNTRL0, _8BIT );
                SETPORT(DATAPORT, *current_SC->SCp.ptr++);
                current_SC->SCp.this_residual--;
              }
            else
              {
                CLRBITS(DMACNTRL0, _8BIT );
                data_count >>= 1; /* Number of words */
                outsw( DATAPORT, current_SC->SCp.ptr, data_count );
                current_SC->SCp.ptr           += 2 * data_count;
                current_SC->SCp.this_residual -= 2 * data_count;
              }

            /* wait for FIFO to get empty */
            while( TESTLO ( DMASTAT, DFIFOEMP|INTSTAT ) )
              ;

#if defined(DEBUG_DATAO)
            printk("fifo (%d bytes), transfered (%d bytes), ",
                   GETPORT(FIFOSTAT), GETSTCNT() );
#endif

            /* if this buffer is empty and there are more buffers left */
            if ( TESTLO( SSTAT1, PHASEMIS ) &&
                 !current_SC->SCp.this_residual &&
                  current_SC->SCp.buffers_residual)
              {
                 /* advance to next buffer */
                 current_SC->SCp.buffers_residual--;
                 current_SC->SCp.buffer++;
                 current_SC->SCp.ptr =
                   current_SC->SCp.buffer->address;
                 current_SC->SCp.this_residual =
                 current_SC->SCp.buffer->length;
              }
          }

        if ( current_SC->SCp.this_residual ||
             current_SC->SCp.buffers_residual )
          {
            /* target leaves DATA OUT for an other phase
               (perhaps disconnect) */

            /* data in fifos has to be resend */
            data_count = GETPORT(SSTAT2) & (SFULL|SFCNT);

            data_count += GETPORT(FIFOSTAT) ;
            current_SC->SCp.ptr           -= data_count;
            current_SC->SCp.this_residual += data_count;
#if defined(DEBUG_DATAO)
            printk("left data (bytes=%d, buffers=%d), fifos (bytes=%d), transfer incomplete, resetting fifo, ",
                   current_SC->SCp.this_residual,
                   current_SC->SCp.buffers_residual,
                   data_count );
#endif
            SETPORT(DMACNTRL0, WRITE_READ|RSTFIFO);
            CLRBITS(SXFRCTL0, SCSIEN|DMAEN );
            CLRBITS(DMACNTRL0, ENDMA);
          }
        else
          {
#if defined(DEBUG_DATAO)
            printk("waiting for SCSI fifo to get empty, ");
#endif
            /* wait for SCSI fifo to get empty */
            while( TESTLO( SSTAT2, SEMPTY ) )
              ;
#if defined(DEBUG_DATAO)
            printk("ok, ");
#endif

#if defined(DEBUG_DATAO)
            printk("left data (bytes=%d, buffers=%d) ",
                   current_SC->SCp.this_residual,
                   current_SC->SCp.buffers_residual);
#endif
            CLRBITS(SXFRCTL0, SCSIEN|DMAEN);

            /* transfer can be considered ended, when SCSIEN reads back zero */
            while( TESTHI( SXFRCTL0, SCSIEN ) )
              ;

            CLRBITS(DMACNTRL0, ENDMA);
          }

#if defined(DEBUG_DATAO) || defined(DEBUG_INTR)
        printk("sent %d data bytes, ", GETSTCNT() );
#endif
      }
      break;

    case P_BUSFREE:                                                /* BUSFREE */
#if defined(DEBUG_RACE)
      leave_driver("(BUSFREE) intr");
#endif
#if defined(DEBUG_PHASES)
      printk("unexpected BUS FREE, ");
#endif
      current_SC->SCp.phase = (current_SC->SCp.phase & ~(P_MASK<<16));

      aha152x_done( DID_ERROR << 16 );               /* Don't know any better */
      return;
      break;

    case P_PARITY:                              /* parity error in DATA phase */
#if defined(DEBUG_RACE)
      leave_driver("(DID_PARITY) intr");
#endif
      printk("PARITY error in DATA phase, ");

      current_SC->SCp.phase = (current_SC->SCp.phase & ~(P_MASK<<16));

      SETBITS( DMACNTRL0, INTEN );
      aha152x_done( DID_PARITY << 16 );
      return;
      break;

    default:
      printk("aha152x: unexpected phase\n");
      break;
    }

  if(done)
    {
#if defined(DEBUG_INTR)
      printk("command done.\n");
#endif
#if defined(DEBUG_RACE)
      leave_driver("(done) intr");
#endif

      SETPORT(SIMODE0, disconnected_SC ? ENSELDI : 0 );
      SETPORT(SIMODE1, issue_SC ? ENBUSFREE : 0);
      SETPORT( SCSISEQ, disconnected_SC ? ENRESELI : 0 );

      SETBITS( DMACNTRL0, INTEN );

      aha152x_done(   (current_SC->SCp.Status  & 0xff)
                    | ( (current_SC->SCp.Message & 0xff) << 8)
                    | ( DID_OK << 16) );

#if defined(DEBUG_RACE)
      printk("done returned (DID_OK: Status=%x; Message=%x).\n",
             current_SC->SCp.Status, current_SC->SCp.Message);
#endif
      return;
    }

  if(current_SC)
    current_SC->SCp.phase |= 1<<16 ;

  SETPORT( SIMODE0, 0 );
  SETPORT( SIMODE1, ENPHASEMIS );
#if defined(DEBUG_INTR)
  disp_enintr();
#endif
#if defined(DEBUG_RACE)
  leave_driver("(PHASEEND) intr");
#endif

  SETBITS( DMACNTRL0, INTEN);
  return;
}

/* 
 * Dump the current driver status and panic...
 */
static void aha152x_panic(char *msg)
{
  printk("\naha152x_panic: %s\n", msg);
  show_queues();
  panic("aha152x panic");
}

/*
 * Display registers of AIC-6260
 */
static void disp_ports(void)
{
#if !defined(SKIP_PORTS)
  int s;

  printk("\n%s: ", current_SC ? "on bus" : "waiting");

  s=GETPORT(SCSISEQ);
  printk("SCSISEQ ( ");
  if( s & TEMODEO )     printk("TARGET MODE ");
  if( s & ENSELO )      printk("SELO ");
  if( s & ENSELI )      printk("SELI ");
  if( s & ENRESELI )    printk("RESELI ");
  if( s & ENAUTOATNO )  printk("AUTOATNO ");
  if( s & ENAUTOATNI )  printk("AUTOATNI ");
  if( s & ENAUTOATNP )  printk("AUTOATNP ");
  if( s & SCSIRSTO )    printk("SCSIRSTO ");
  printk(");");

  printk(" SCSISIG ( ");
  s=GETPORT(SCSISIG);
  switch(s & P_MASK)
    {
    case P_DATAO:
      printk("DATA OUT");
      break;
    case P_DATAI:
      printk("DATA IN");
      break;
    case P_CMD:
      printk("COMMAND"); 
      break;
    case P_STATUS:
      printk("STATUS"); 
      break;
    case P_MSGO:
      printk("MESSAGE OUT");
      break;
    case P_MSGI:
      printk("MESSAGE IN");
      break;
    default:
      printk("*illegal*");
      break;
    }
  
  printk(" ); ");

  printk("INTSTAT ( %s ); ", TESTHI(DMASTAT, INTSTAT) ? "hi" : "lo");

  printk("SSTAT ( ");
  s=GETPORT(SSTAT0);
  if( s & TARGET )   printk("TARGET ");
  if( s & SELDO )    printk("SELDO ");
  if( s & SELDI )    printk("SELDI ");
  if( s & SELINGO )  printk("SELINGO ");
  if( s & SWRAP )    printk("SWRAP ");
  if( s & SDONE )    printk("SDONE ");
  if( s & SPIORDY )  printk("SPIORDY ");
  if( s & DMADONE )  printk("DMADONE ");

  s=GETPORT(SSTAT1);
  if( s & SELTO )     printk("SELTO ");
  if( s & ATNTARG )   printk("ATNTARG ");
  if( s & SCSIRSTI )  printk("SCSIRSTI ");
  if( s & PHASEMIS )  printk("PHASEMIS ");
  if( s & BUSFREE )   printk("BUSFREE ");
  if( s & SCSIPERR )  printk("SCSIPERR ");
  if( s & PHASECHG )  printk("PHASECHG ");
  if( s & REQINIT )   printk("REQINIT ");
  printk("); ");


  printk("SSTAT ( ");

  s=GETPORT(SSTAT0) & GETPORT(SIMODE0);

  if( s & TARGET )    printk("TARGET ");
  if( s & SELDO )     printk("SELDO ");
  if( s & SELDI )     printk("SELDI ");
  if( s & SELINGO )   printk("SELINGO ");
  if( s & SWRAP )     printk("SWRAP ");
  if( s & SDONE )     printk("SDONE ");
  if( s & SPIORDY )   printk("SPIORDY ");
  if( s & DMADONE )   printk("DMADONE ");

  s=GETPORT(SSTAT1) & GETPORT(SIMODE1);

  if( s & SELTO )     printk("SELTO ");
  if( s & ATNTARG )   printk("ATNTARG ");
  if( s & SCSIRSTI )  printk("SCSIRSTI ");
  if( s & PHASEMIS )  printk("PHASEMIS ");
  if( s & BUSFREE )   printk("BUSFREE ");
  if( s & SCSIPERR )  printk("SCSIPERR ");
  if( s & PHASECHG )  printk("PHASECHG ");
  if( s & REQINIT )   printk("REQINIT ");
  printk("); ");

  printk("SXFRCTL0 ( ");

  s=GETPORT(SXFRCTL0);
  if( s & SCSIEN )    printk("SCSIEN ");
  if( s & DMAEN )     printk("DMAEN ");
  if( s & CH1 )       printk("CH1 ");
  if( s & CLRSTCNT )  printk("CLRSTCNT ");
  if( s & SPIOEN )    printk("SPIOEN ");
  if( s & CLRCH1 )    printk("CLRCH1 ");
  printk("); ");

  printk("SIGNAL ( ");

  s=GETPORT(SCSISIG);
  if( s & ATNI )  printk("ATNI ");
  if( s & SELI )  printk("SELI ");
  if( s & BSYI )  printk("BSYI ");
  if( s & REQI )  printk("REQI ");
  if( s & ACKI )  printk("ACKI ");
  printk("); ");

  printk("SELID ( %02x ), ", GETPORT(SELID) );

  printk("SSTAT2 ( ");

  s=GETPORT(SSTAT2);
  if( s & SOFFSET)  printk("SOFFSET ");
  if( s & SEMPTY)   printk("SEMPTY ");
  if( s & SFULL)    printk("SFULL ");
  printk("); SFCNT ( %d ); ", s & (SFULL|SFCNT) );

#if 0
  printk("SSTAT4 ( ");
  s=GETPORT(SSTAT4);
  if( s & SYNCERR)   printk("SYNCERR ");
  if( s & FWERR)     printk("FWERR ");
  if( s & FRERR)     printk("FRERR ");
  printk("); ");
#endif

  printk("FCNT ( %d ); ", GETPORT(FIFOSTAT) );

  printk("DMACNTRL0 ( ");
  s=GETPORT(DMACNTRL0);
  printk( "%s ", s & _8BIT      ? "8BIT"  : "16BIT" );
  printk( "%s ", s & DMA        ? "DMA"   : "PIO"   );
  printk( "%s ", s & WRITE_READ ? "WRITE" : "READ"  );
  if( s & ENDMA )    printk("ENDMA ");
  if( s & INTEN )    printk("INTEN ");
  if( s & RSTFIFO )  printk("RSTFIFO ");
  if( s & SWINT )    printk("SWINT ");
  printk("); ");


#if 0
  printk("DMACNTRL1 ( ");

  s=GETPORT(DMACNTRL1);
  if( s & PWRDWN )    printk("PWRDN ");
  printk("); ");


  printk("STK ( %d ); ", s & 0xf);

  printk("DMASTAT (");
  s=GETPORT(DMASTAT);
  if( s & ATDONE )     printk("ATDONE ");
  if( s & WORDRDY )    printk("WORDRDY ");
  if( s & DFIFOFULL )  printk("DFIFOFULL ");
  if( s & DFIFOEMP )   printk("DFIFOEMP ");
  printk(")");

#endif

  printk("\n");
#endif
}

/*
 * display enabled interrupts
 */
static void disp_enintr(void)
{
  int s;

  printk("enabled interrupts ( ");
  
  s=GETPORT(SIMODE0);
  if( s & ENSELDO )    printk("ENSELDO ");
  if( s & ENSELDI )    printk("ENSELDI ");
  if( s & ENSELINGO )  printk("ENSELINGO ");
  if( s & ENSWRAP )    printk("ENSWRAP ");
  if( s & ENSDONE )    printk("ENSDONE ");
  if( s & ENSPIORDY )  printk("ENSPIORDY ");
  if( s & ENDMADONE )  printk("ENDMADONE ");

  s=GETPORT(SIMODE1);
  if( s & ENSELTIMO )    printk("ENSELTIMO ");
  if( s & ENATNTARG )    printk("ENATNTARG ");
  if( s & ENPHASEMIS )   printk("ENPHASEMIS ");
  if( s & ENBUSFREE )    printk("ENBUSFREE ");
  if( s & ENSCSIPERR )   printk("ENSCSIPERR ");
  if( s & ENPHASECHG )   printk("ENPHASECHG ");
  if( s & ENREQINIT )    printk("ENREQINIT ");
  printk(")\n");
}

#if defined(DEBUG_RACE)

static const char *should_leave;
static int in_driver=0;

/*
 * Only one routine can be in the driver at once.
 */
static void enter_driver(const char *func)
{
  cli();
  printk("aha152x: entering %s() (%x)\n", func, jiffies);
  if(in_driver)
    {
      printk("%s should leave first.\n", should_leave);
      panic("aha152x: already in driver\n");
    }

  in_driver++;
  should_leave=func;
  sti();
}

static void leave_driver(const char *func)
{
  cli();
  printk("\naha152x: leaving %s() (%x)\n", func, jiffies);
  if(!in_driver)
    {
      printk("aha152x: %s already left.\n", should_leave);
      panic("aha152x: %s already left driver.\n");
    }

  in_driver--;
  should_leave=func;
  sti();
}
#endif

/*
 * Show the command data of a command
 */
static void show_command(Scsi_Cmnd *ptr)
{
  int i;

  printk("0x%08x: target=%d; lun=%d; cmnd=( ",
         (unsigned long) ptr, ptr->target, ptr->lun);
  
  for(i=0; i<COMMAND_SIZE(ptr->cmnd[0]); i++)
    printk("%02x ", ptr->cmnd[i]);

  printk("); residual=%d; buffers=%d; phase |",
         ptr->SCp.this_residual, ptr->SCp.buffers_residual);

  if( ptr->SCp.phase & not_issued   )  printk("not issued|");
  if( ptr->SCp.phase & in_selection )  printk("in selection|");
  if( ptr->SCp.phase & disconnected )  printk("disconnected|");
  if( ptr->SCp.phase & aborted      )  printk("aborted|");
  if( ptr->SCp.phase & sent_ident   )  printk("send_ident|");
  if( ptr->SCp.phase & in_other )
    { 
      printk("; in other(");
      switch( (ptr->SCp.phase >> 16) & P_MASK )
        {
        case P_DATAO:
          printk("DATA OUT");
          break;
        case P_DATAI:
          printk("DATA IN");
          break;
        case P_CMD:
          printk("COMMAND");
          break;
        case P_STATUS:
          printk("STATUS");
          break;
        case P_MSGO:
          printk("MESSAGE OUT");
          break;
        case P_MSGI:
          printk("MESSAGE IN");
          break;
        default: 
          printk("*illegal*");
          break;
        }
      printk(")");
      if(ptr->SCp.phase & (1<<16))
        printk("; phaseend");
    }
  printk("; next=0x%08x\n", (unsigned long) ptr->host_scribble);
}
 
/*
 * Dump the queued data
 */
static void show_queues(void)
{
  Scsi_Cmnd *ptr;

  cli();
  printk("QUEUE STATUS:\nissue_SC:\n");
  for(ptr=issue_SC; ptr; ptr = (Scsi_Cmnd *) ptr->host_scribble )
    show_command(ptr);

  printk("current_SC:\n");
  if(current_SC)
    show_command(current_SC);
  else
    printk("none\n");

  printk("disconnected_SC:\n");
  for(ptr=disconnected_SC; ptr; ptr = (Scsi_Cmnd *) ptr->host_scribble )
    show_command(ptr);

  disp_ports();
  disp_enintr();
  sti();
}
