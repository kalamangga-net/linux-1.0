/* 
 * ASCII values for a number of symbolic constants, printing functions,
 * etc.
 */

#include <linux/config.h>
#include "../block/blk.h"
#include <linux/kernel.h>
#include "scsi.h"

#define CONST_COMMAND 	0x01
#define CONST_STATUS 	0x02
#define CONST_SENSE 	0x04
#define CONST_XSENSE 	0x08
static const char unknown[] = "UNKNOWN";

#ifdef CONFIG_SCSI_CONSTANTS
#ifdef CONSTANTS
#undef CONSTANTS
#endif
#define CONSTANTS (CONST_COMMAND | CONST_STATUS | CONST_SENSE | CONST_XSENSE)
#endif

#if (CONSTANTS & CONST_COMMAND)
static const char * group_0_commands[] = {
/* 00-03 */ "Test Unit Ready", "Rezero Unit", unknown, "Request Sense",
/* 04-07 */ "Format Unit", "Read Block Limits", unknown, "Reasssign Blocks",
/* 08-0d */ "Read (6)", unknown, "Write (6)", "Seek (6)", unknown, unknown,
/* 0e-12 */ unknown, "Read Reverse", "Write Filemarks", "Space", "Inquiry",  
/* 13-16 */ unknown, "Recover Buffered Data", "Mode Select", "Reserve",
/* 17-1b */ "Release", "Copy", "Erase", "Mode Sense", "Start/Stop Unit",
/* 1c-1d */ "Receive Diagnostic", "Send Diagnostic", 
/* 1e-1f */ "Prevent/Allow Medium Removal", unknown,
};


static const char *group_1_commands[] = {
/* 20-22 */  unknown, unknown, unknown,
/* 23-28 */ unknown, unknown, "Read Capacity", unknown, unknown, "Read (10)", 
/* 29-2d */ unknown, "Write (10)", "Seek (10)", unknown, unknown, 
/* 2e-31 */ "Write Verify","Verify", "Search High", "Search Equal", 
/* 32-34 */ "Search Low", "Set Limits", "Prefetch or Read Position", 
/* 35-37 */ "Synchronize Cache","Lock/Unlock Cache", "Read Deffect Data", 
/* 38-3c */ unknown, "Compare","Copy Verify", "Write Buffer", "Read Buffer", 
/* 3d-39 */ unknown, "Read Long",  unknown,
};


static const char *group_2_commands[] = {
/* 40-41 */ "Change Definition", unknown, 
/* 42-48 */ unknown, unknown, unknown, unknown, unknown, unknown, unknown,
/* 49-4f */ unknown, unknown, unknown, "Log Select", "Log Sense", unknown,
/* 50-55 */ unknown, unknown, unknown, unknown, unknown, "Mode Select (10)",
/* 56-5b */ unknown, unknown, unknown, unknown, "Mode Sense (10)", unknown,
/* 5c-5f */ unknown, unknown, unknown,
};



#define group(opcode) (((opcode) >> 5) & 7)

#define RESERVED_GROUP  0
#define VENDOR_GROUP 	1
#define NOTEXT_GROUP	2

static const char **commands[] = {
group_0_commands, group_1_commands, group_2_commands, 
(const char **) RESERVED_GROUP, (const char **) RESERVED_GROUP, 
(const char **) NOTEXT_GROUP, (const char **) VENDOR_GROUP, 
(const char **) VENDOR_GROUP};

static const char reserved[] = "RESERVED";
static const char vendor[] = "VENDOR SPECIFIC";

static void print_opcode(int opcode) {
  char **table = commands[ group(opcode) ];
  switch ((int) table) {
  case RESERVED_GROUP:
  	printk("%s(0x%02x) ", reserved, opcode); 
  	break;
  case NOTEXT_GROUP:
  	printk("%s(0x%02x) ", unknown, opcode); 
  	break;
  case VENDOR_GROUP:
  	printk("%s(0x%02x) ", vendor, opcode); 
  	break;
  default:
  	printk("%s ",table[opcode & 0x31]);
  }
}
#else /* CONST & CONST_COMMAND */
static void print_opcode(int opcode) {
  printk("0x%02x ", opcode);
}
#endif  

void print_command (unsigned char *command) {
  int i,s;
  print_opcode(command[0]);
  for ( i = 1, s = COMMAND_SIZE(command[0]); i < s; ++i) 
  	printk("%02x ", command[i]);
  printk("\n");
}

#if (CONSTANTS & CONST_STATUS)
static const char * statuses[] = {
/* 0-4 */ "Good", "Check Condition", "Condition Good", unknown, "Busy", 
/* 5-9 */ unknown, unknown, unknown, "Intermediate Good", unknown, 
/* a-d */ "Interemediate Good", unknown, "Reservation Conflict", unknown,
/* e-f */ unknown, unknown,
};
#endif

void print_status (int status) {
  status = (status >> 1) & 0xf;
#if (CONSTANTS & CONST_STATUS)
  printk("%s ",statuses[status]);
#else
  printk("0x%0x ", status); 
#endif 
}

#if (CONSTANTS & CONST_XSENSE)
#define D 0x001  /* DIRECT ACCESS DEVICE (disk) */
#define T 0x002  /* SEQUENTIAL ACCESS DEVICE (tape) */
#define L 0x004  /* PRINTER DEVICE */
#define P 0x008  /* PROCESSOR DEVICE */
#define W 0x010  /* WRITE ONCE READ MULTIPLE DEVICE */
#define R 0x020  /* READ ONLY (CD-ROM) DEVICE */
#define S 0x040  /* SCANNER DEVICE */
#define O 0x080  /* OPTICAL MEMORY DEVICE */
#define M 0x100  /* MEDIA CHANGER DEVICE */
#define C 0x200  /* COMMUNICATION DEVICE */

struct error_info{
  unsigned char code1, code2;
  unsigned short int devices;
  char * text;
};

struct error_info2{
  unsigned char code1, code2_min, code2_max;
  unsigned short int devices;
  char * text;
};

static struct error_info2 additional2[] =
{
  {0x40,0x00,0x7f,D,"Ram failure (%x)"},
  {0x40,0x80,0xff,D|T|L|P|W|R|S|O|M|C,"Diagnostic failure on component (%x)"},
  {0x41,0x00,0xff,D,"Data path failure (%x)"},
  {0x42,0x00,0xff,D,"Power-on or self-test failure (%x)"},
  {0, 0, 0, 0, NULL}
};

static struct error_info additional[] =
{
  {0x00,0x01,T,"Filemark detected"},
  {0x00,0x02,T|S,"End-of-partition/medium detected"},
  {0x00,0x03,T,"Setmark detected"},
  {0x00,0x04,T|S,"Beginning-of-partition/medium detected"},
  {0x00,0x05,T|S,"End-of-data detected"},
  {0x00,0x06,D|T|L|P|W|R|S|O|M|C,"I/O process terminated"},
  {0x00,0x11,R,"Audio play operation in progress"},
  {0x00,0x12,R,"Audio play operation paused"},
  {0x00,0x13,R,"Audio play operation successfully completed"},
  {0x00,0x14,R,"Audio play operation stopped due to error"},
  {0x00,0x15,R,"No current audio status to return"},
  {0x01,0x00,D|W|O,"No index/sector signal"},
  {0x02,0x00,D|W|R|O|M,"No seek complete"},
  {0x03,0x00,D|T|L|W|S|O,"Peripheral device write fault"},
  {0x03,0x01,T,"No write current"},
  {0x03,0x02,T,"Excessive write errors"},
  {0x04,0x00,D|T|L|P|W|R|S|O|M|C,
     "Logical unit not ready, cause not reportable"},
  {0x04,0x01,D|T|L|P|W|R|S|O|M|C,
     "Logical unit is in process of becoming ready"},
  {0x04,0x02,D|T|L|P|W|R|S|O|M|C,
     "Logical unit not ready, initializing command required"},
  {0x04,0x03,D|T|L|P|W|R|S|O|M|C,
     "Logical unit not ready, manual intervention required"},
  {0x04,0x04,D|T|L|O,"Logical unit not ready, format in progress"},
  {0x05,0x00,D|T|L|W|R|S|O|M|C,"Logical unit does not respond to selection"},
  {0x06,0x00,D|W|R|O|M,"No reference position found"},
  {0x07,0x00,D|T|L|W|R|S|O|M,"Multiple peripheral devices selected"},
  {0x08,0x00,D|T|L|W|R|S|O|M|C,"Logical unit communication failure"},
  {0x08,0x01,D|T|L|W|R|S|O|M|C,"Logical unit communication time-out"},
  {0x08,0x02,D|T|L|W|R|S|O|M|C,"Logical unit communication parity error"},
  {0x09,0x00,D|T|W|R|O,"Track following error"},
  {0x09,0x01,W|R|O,"Tracking servo failure"},
  {0x09,0x02,W|R|O,"Focus servo failure"},
  {0x09,0x03,W|R|O,"Spindle servo failure"},
  {0x0A,0x00,D|T|L|P|W|R|S|O|M|C,"Error log overflow"},
  {0x0C,0x00,T|S,"Write error"},
  {0x0C,0x01,D|W|O,"Write error recovered with auto reallocation"},
  {0x0C,0x02,D|W|O,"Write error - auto reallocation failed"},
  {0x10,0x00,D|W|O,"Id crc or ecc error"},
  {0x11,0x00,D|T|W|R|S|O,"Unrecovered read error"},
  {0x11,0x01,D|T|W|S|O,"Read retries exhausted"},
  {0x11,0x02,D|T|W|S|O,"Error too long to correct"},
  {0x11,0x03,D|T|W|S|O,"Multiple read errors"},
  {0x11,0x04,D|W|O,"Unrecovered read error - auto reallocate failed"},
  {0x11,0x05,W|R|O,"L-ec uncorrectable error"},
  {0x11,0x06,W|R|O,"Circ unrecovered error"},
  {0x11,0x07,W|O,"Data resychronization error"},
  {0x11,0x08,T,"Incomplete block read"},
  {0x11,0x09,T,"No gap found"},
  {0x11,0x0A,D|T|O,"Miscorrected error"},
  {0x11,0x0B,D|W|O,"Unrecovered read error - recommend reassignment"},
  {0x11,0x0C,D|W|O,"Unrecovered read error - recommend rewrite the data"},
  {0x12,0x00,D|W|O,"Address mark not found for id field"},
  {0x13,0x00,D|W|O,"Address mark not found for data field"},
  {0x14,0x00,D|T|L|W|R|S|O,"Recorded entity not found"},
  {0x14,0x01,D|T|W|R|O,"Record not found"},
  {0x14,0x02,T,"Filemark or setmark not found"},
  {0x14,0x03,T,"End-of-data not found"},
  {0x14,0x04,T,"Block sequence error"},
  {0x15,0x00,D|T|L|W|R|S|O|M,"Random positioning error"},
  {0x15,0x01,D|T|L|W|R|S|O|M,"Mechanical positioning error"},
  {0x15,0x02,D|T|W|R|O,"Positioning error detected by read of medium"},
  {0x16,0x00,D|W|O,"Data synchronization mark error"},
  {0x17,0x00,D|T|W|R|S|O,"Recovered data with no error correction applied"},
  {0x17,0x01,D|T|W|R|S|O,"Recovered data with retries"},
  {0x17,0x02,D|T|W|R|O,"Recovered data with positive head offset"},
  {0x17,0x03,D|T|W|R|O,"Recovered data with negative head offset"},
  {0x17,0x04,W|R|O,"Recovered data with retries and/or circ applied"},
  {0x17,0x05,D|W|R|O,"Recovered data using previous sector id"},
  {0x17,0x06,D|W|O,"Recovered data without ecc - data auto-reallocated"},
  {0x17,0x07,D|W|O,"Recovered data without ecc - recommend reassignment"},
  {0x18,0x00,D|T|W|R|O,"Recovered data with error correction applied"},
  {0x18,0x01,D|W|R|O,"Recovered data with error correction and retries applied"},
  {0x18,0x02,D|W|R|O,"Recovered data - data auto-reallocated"},
  {0x18,0x03,R,"Recovered data with circ"},
  {0x18,0x04,R,"Recovered data with lec"},
  {0x18,0x05,D|W|R|O,"Recovered data - recommend reassignment"},
  {0x19,0x00,D|O,"Defect list error"},
  {0x19,0x01,D|O,"Defect list not available"},
  {0x19,0x02,D|O,"Defect list error in primary list"},
  {0x19,0x03,D|O,"Defect list error in grown list"},
  {0x1A,0x00,D|T|L|P|W|R|S|O|M|C,"Parameter list length error"},
  {0x1B,0x00,D|T|L|P|W|R|S|O|M|C,"Synchronous data transfer error"},
  {0x1C,0x00,D|O,"Defect list not found"},
  {0x1C,0x01,D|O,"Primary defect list not found"},
  {0x1C,0x02,D|O,"Grown defect list not found"},
  {0x1D,0x00,D|W|O,"Miscompare during verify operation"},
  {0x1E,0x00,D|W|O,"Recovered id with ecc correction"},
  {0x20,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid command operation code"},
  {0x21,0x00,D|T|W|R|O|M,"Logical block address out of range"},
  {0x21,0x01,M,"Invalid element address"},
  {0x22,0x00,D,"Illegal function (should use 20 00, 24 00, or 26 00)"},
  {0x24,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid field in cdb"},
  {0x25,0x00,D|T|L|P|W|R|S|O|M|C,"Logical unit not supported"},
  {0x26,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid field in parameter list"},
  {0x26,0x01,D|T|L|P|W|R|S|O|M|C,"Parameter not supported"},
  {0x26,0x02,D|T|L|P|W|R|S|O|M|C,"Parameter value invalid"},
  {0x26,0x03,D|T|L|P|W|R|S|O|M|C,"Threshold parameters not supported"},
  {0x27,0x00,D|T|W|O,"Write protected"},
  {0x28,0x00,D|T|L|P|W|R|S|O|M|C,"Not ready to ready transition (medium may have changed)"},
  {0x28,0x01,M,"Import or export element accessed"},
  {0x29,0x00,D|T|L|P|W|R|S|O|M|C,"Power on, reset, or bus device reset occurred"},
  {0x2A,0x00,D|T|L|W|R|S|O|M|C,"Parameters changed"},
  {0x2A,0x01,D|T|L|W|R|S|O|M|C,"Mode parameters changed"},
  {0x2A,0x02,D|T|L|W|R|S|O|M|C,"Log parameters changed"},
  {0x2B,0x00,D|T|L|P|W|R|S|O|C,"Copy cannot execute since host cannot disconnect"},
  {0x2C,0x00,D|T|L|P|W|R|S|O|M|C,"Command sequence error"},
  {0x2C,0x01,S,"Too many windows specified"},
  {0x2C,0x02,S,"Invalid combination of windows specified"},
  {0x2D,0x00,T,"Overwrite error on update in place"},
  {0x2F,0x00,D|T|L|P|W|R|S|O|M|C,"Commands cleared by another initiator"},
  {0x30,0x00,D|T|W|R|O|M,"Incompatible medium installed"},
  {0x30,0x01,D|T|W|R|O,"Cannot read medium - unknown format"},
  {0x30,0x02,D|T|W|R|O,"Cannot read medium - incompatible format"},
  {0x30,0x03,D|T,"Cleaning cartridge installed"},
  {0x31,0x00,D|T|W|O,"Medium format corrupted"},
  {0x31,0x01,D|L|O,"Format command failed"},
  {0x32,0x00,D|W|O,"No defect spare location available"},
  {0x32,0x01,D|W|O,"Defect list update failure"},
  {0x33,0x00,T,"Tape length error"},
  {0x36,0x00,L,"Ribbon, ink, or toner failure"},
  {0x37,0x00,D|T|L|W|R|S|O|M|C,"Rounded parameter"},
  {0x39,0x00,D|T|L|W|R|S|O|M|C,"Saving parameters not supported"},
  {0x3A,0x00,D|T|L|W|R|S|O|M,"Medium not present"},
  {0x3B,0x00,T|L,"Sequential positioning error"},
  {0x3B,0x01,T,"Tape position error at beginning-of-medium"},
  {0x3B,0x02,T,"Tape position error at end-of-medium"},
  {0x3B,0x03,L,"Tape or electronic vertical forms unit not ready"},
  {0x3B,0x04,L,"Slew failure"},
  {0x3B,0x05,L,"Paper jam"},
  {0x3B,0x06,L,"Failed to sense top-of-form"},
  {0x3B,0x07,L,"Failed to sense bottom-of-form"},
  {0x3B,0x08,T,"Reposition error"},
  {0x3B,0x09,S,"Read past end of medium"},
  {0x3B,0x0A,S,"Read past beginning of medium"},
  {0x3B,0x0B,S,"Position past end of medium"},
  {0x3B,0x0C,S,"Position past beginning of medium"},
  {0x3B,0x0D,M,"Medium destination element full"},
  {0x3B,0x0E,M,"Medium source element empty"},
  {0x3D,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid bits in identify message"},
  {0x3E,0x00,D|T|L|P|W|R|S|O|M|C,"Logical unit has not self-configured yet"},
  {0x3F,0x00,D|T|L|P|W|R|S|O|M|C,"Target operating conditions have changed"},
  {0x3F,0x01,D|T|L|P|W|R|S|O|M|C,"Microcode has been changed"},
  {0x3F,0x02,D|T|L|P|W|R|S|O|M|C,"Changed operating definition"},
  {0x3F,0x03,D|T|L|P|W|R|S|O|M|C,"Inquiry data has changed"},
  {0x43,0x00,D|T|L|P|W|R|S|O|M|C,"Message error"},
  {0x44,0x00,D|T|L|P|W|R|S|O|M|C,"Internal target failure"},
  {0x45,0x00,D|T|L|P|W|R|S|O|M|C,"Select or reselect failure"},
  {0x46,0x00,D|T|L|P|W|R|S|O|M|C,"Unsuccessful soft reset"},
  {0x47,0x00,D|T|L|P|W|R|S|O|M|C,"Scsi parity error"},
  {0x48,0x00,D|T|L|P|W|R|S|O|M|C,"Initiator detected error message received"},
  {0x49,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid message error"},
  {0x4A,0x00,D|T|L|P|W|R|S|O|M|C,"Command phase error"},
  {0x4B,0x00,D|T|L|P|W|R|S|O|M|C,"Data phase error"},
  {0x4C,0x00,D|T|L|P|W|R|S|O|M|C,"Logical unit failed self-configuration"},
  {0x4E,0x00,D|T|L|P|W|R|S|O|M|C,"Overlapped commands attempted"},
  {0x50,0x00,T,"Write append error"},
  {0x50,0x01,T,"Write append position error"},
  {0x50,0x02,T,"Position error related to timing"},
  {0x51,0x00,T|O,"Erase failure"},
  {0x52,0x00,T,"Cartridge fault"},
  {0x53,0x00,D|T|L|W|R|S|O|M,"Media load or eject failed"},
  {0x53,0x01,T,"Unload tape failure"},
  {0x53,0x02,D|T|W|R|O|M,"Medium removal prevented"},
  {0x54,0x00,P,"Scsi to host system interface failure"},
  {0x55,0x00,P,"System resource failure"},
  {0x57,0x00,R,"Unable to recover table-of-contents"},
  {0x58,0x00,O,"Generation does not exist"},
  {0x59,0x00,O,"Updated block read"},
  {0x5A,0x00,D|T|L|P|W|R|S|O|M,"Operator request or state change input (unspecified)"},
  {0x5A,0x01,D|T|W|R|O|M,"Operator medium removal request"},
  {0x5A,0x02,D|T|W|O,"Operator selected write protect"},
  {0x5A,0x03,D|T|W|O,"Operator selected write permit"},
  {0x5B,0x00,D|T|L|P|W|R|S|O|M,"Log exception"},
  {0x5B,0x01,D|T|L|P|W|R|S|O|M,"Threshold condition met"},
  {0x5B,0x02,D|T|L|P|W|R|S|O|M,"Log counter at maximum"},
  {0x5B,0x03,D|T|L|P|W|R|S|O|M,"Log list codes exhausted"},
  {0x5C,0x00,D|O,"Rpl status change"},
  {0x5C,0x01,D|O,"Spindles synchronized"},
  {0x5C,0x02,D|O,"Spindles not synchronized"},
  {0x60,0x00,S,"Lamp failure"},
  {0x61,0x00,S,"Video acquisition error"},
  {0x61,0x01,S,"Unable to acquire video"},
  {0x61,0x02,S,"Out of focus"},
  {0x62,0x00,S,"Scan head positioning error"},
  {0x63,0x00,R,"End of user area encountered on this track"},
  {0x64,0x00,R,"Illegal mode for this track"},
  {0, 0, 0, NULL}
};
#endif

#if (CONSTANTS & CONST_SENSE)
static char *snstext[] = {
	"None","Recovered Error","Not Ready","Medium Error","Hardware Error",
	"Illegal Request","Unit Attention","Data Protect","Blank Check",
	"Key=E","Key=F","Filemark","End-Of-Medium","Incorrect Block Length",
	"14","15"};
#endif


/* Print sense information */
void print_sense(char * devclass, Scsi_Cmnd * SCpnt)
{
	int i, s;
	int sense_class, valid, code;
	unsigned char * sense_buffer = SCpnt->sense_buffer;
	char * error = NULL;
	int dev = SCpnt->request.dev;

	sense_class = (sense_buffer[0] >> 4) & 0x07;
	code = sense_buffer[0] & 0xf;
	valid = sense_buffer[0] & 0x80;

	if (sense_class == 7) { 
	  s = sense_buffer[7] + 8;
	  if(s > sizeof(SCpnt->sense_buffer)) s = sizeof(SCpnt->sense_buffer);

	  if (!valid)
	    printk("extra data not valid ");
	  
	  if (sense_buffer[2] & 0x80) printk( "FMK ");
	  if (sense_buffer[2] & 0x40) printk( "EOM ");
	  if (sense_buffer[2] & 0x20) printk( "ILI ");

	  switch (code) {
	  case 0x0:
	    error = "Current";
	    break;
	  case 0x1:
	    error = "Deferred";
	    break;
	  default:
	    error = "Invalid";
	  }
	  
	  printk("%s error ", error);
	  
#if (CONSTANTS & CONST_SENSE)
	  if (sense_buffer[2] & 0x80) printk( "FMK ");
	  if (sense_buffer[2] & 0x40) printk( "EOM ");
	  if (sense_buffer[2] & 0x20) printk( "ILI ");
	  printk( "%s%x: sense key %s\n", devclass, dev, snstext[sense_buffer[2] & 0x0f]);
#else
	  printk("%s%x: sns = %2x %2x\n", devclass, dev, sense_buffer[0], sense_buffer[2]);
#endif
	
	/* Check to see if additional sense information is available */
	if(sense_buffer[7] + 7 < 13 ||
	   (sense_buffer[12] == 0  && sense_buffer[13] ==  0)) goto done;
	
#if (CONSTANTS & CONST_XSENSE)
	for(i=0; additional[i].text; i++)
		if(additional[i].code1 == sense_buffer[12] &&
		   additional[i].code2 == sense_buffer[13])
			printk("Additional sense indicates %s\n", additional[i].text);
	
	for(i=0; additional2[i].text; i++)
		if(additional2[i].code1 == sense_buffer[12] &&
		   additional2[i].code2_min >= sense_buffer[13]  &&
		   additional2[i].code2_max <= sense_buffer[13]) {
			printk("Additional sense indicates ");
			printk(additional2[i].text, sense_buffer[13]);
			printk("\n");
		};
#else
	printk("ASC=%2x ASCQ=%2x\n", sense_buffer[12], sense_buffer[13]);
#endif
	} else { 

#if (CONSTANTS & CONST_SENSE)
	  if (sense_buffer[0] < 15)
	    printk("%s%x: old sense key %s\n", devclass, dev, snstext[sense_buffer[0] & 0x0f]);
	  else
#endif
	    printk("%s%x: sns = %2x %2x\n", devclass, dev, sense_buffer[0], sense_buffer[2]);

	  printk("Non-extended sense class %d code 0x%0x ", sense_class, code);
	  s = 4;
	}
	
      done:
	for (i = 0; i < s; ++i) 
	  printk("0x%02x ", sense_buffer[i]);

	return;
}

#if (CONSTANTS & CONST_MSG) 
static const char *one_byte_msgs[] = {
/* 0x00 */ "Command Complete", NULL, "Save Pointers",
/* 0x03 */ "Restore Pointers", "Disconnect", "Initiator Error", 
/* 0x06 */ "Abort", "Message Reject", "Nop", "Message Parity Error",
/* 0x0a */ "Linked Command Complete", "Linked Command Complete w/flag",
/* 0x0c */ "Bus device reset", "Abort Tag", "Clear Queue", 
/* 0x0f */ "Initiate Recovery", "Release Recovery"
}

#define NO_ONE_BYTE_MSGS (sizeof(one_byte_msgs)  / sizeof (const char *))

static const char *queue_tag_msgs[] = {
/* 0x20 */ "Simple Queue Tag", "Head of Queue Tag", "Ordered Queue Tag"
/* 0x23 */ "Ignore Wide Residue"
}

#define NO_TWO_BYTE_MSGS (sizeof(two_byte_msgs)  / sizeof (const char *))

static const char *extended_msgs[] = {
/* 0x00 */ "Modify Data Pointer", "Synchronous Data Transfer Request",
/* 0x02 */ "SCSI-I Extended Identify", "Wide Data Transfer Reqeust"
};

#define NO_EXTENDED_MSGS (sizeof(two_byte_msgs)  / sizeof (const char *))
#endif /* (CONSTANTS & CONST_MSG) */

int print_msg (const unsigned char *msg) {
    int len = 0, i;
    if (msg[0] == EXTENDED_MESSAGE) {
	len = 3 + msg[1];
#if (CONSTANTS & CONST_MSG)
	printk("Extended Message code %s arguments ", 
	    (msg[2] < NO_EXTENDED_MESSAGES) ?
	    printk("%s " extended_msgs[msg[2]]),
	    reserved);
	for (i = 3; i < msg[1]; ++i) 
#else
	for (i = 0; i < msg[1]; ++i)
#endif
	    printk("%02x ", msg[i]);
    /* Identify */
    } else if (msg[0] & 0x80) {
#if (CONSTANTS & CONST_MSG)
	printk("Identify disconnect %sallowed %s %d ",
	    (msg[0] & 0x40) ? "" : "not ",
	    (msg[0] & 0x20) ? "target routine" : "lun",
	    msg[0] & 0x7);
#else
    printk("%02x ", msg[0]);
#endif
    len = 1;
    /* Normal One byte */
    } else if (msg[0] < 0x1f) {
#if (CONSTANTS & CONST_MSG)
	if (msg[0] < NO_ONE_BYTE_MSGS)
	    printk(one_byte_msgs[msg[0]]);
	else
	    printk("reserved (%02x) ", msg[0]);
#else
	printk("%02x ", msg[0]);
#endif
	len = 1;
    /* Two byte */
    } else if (msg[0] <= 0x2f) {
#if (CONSTANTS & CONST_MSG)
	if ((msg[0] - 0x20) < NO_TWO_BYTE_MESSAGES) 
	    printk("%s %02x ", two_byte_msgs[msg[0] - 0x20], 
		msg[1]);
	else 
	    printk("reserved two byte (%02x %02x) ", 
		msg[0], msg[1]);
#else
	printk("%02x %02x", msg[0], msg[1]);
#endif
	len = 2;
    } else 
#if (CONSTANTS & CONST_MSG)
	printk(reserved);
#else
	printk("%02x ", msg[0]);
#endif
    return len;
}
