/*
   History:
    Started: Aug 9 by Lawrence Foard (entropy@world.std.com), to allow user 
     process control of SCSI devices.
    Development Sponsored by Killy Corp. NY NY
*/

/* 
 An SG device is accessed by writting "packets" to it, the replies
 are then read using the read call. The same header is used for 
 reply, just ignore reply_len field.
*/

struct sg_header
 {
  int pack_len;    /* length of incoming packet <4096 (including header) */
  int reply_len;   /* maximum length <4096 of expected reply */
  int pack_id;     /* id number of packet */
  int result;      /* 0==ok, otherwise refer to errno codes */
  /* command follows then data for command */
 };

/* ioctl's */
#define SG_SET_TIMEOUT 0x2201  /* set timeout *(int *)arg==timeout */
#define SG_GET_TIMEOUT 0x2202  /* get timeout return timeout */

#define SG_DEFAULT_TIMEOUT 6000 /* 1 minute timeout */
#define SG_DEFAULT_RETRIES 1

#define SG_MAX_QUEUE 4 /* maximum outstanding request, arbitrary, may be
                          changed if sufficient DMA buffer room available */

#define SG_BIG_BUFF 32768
