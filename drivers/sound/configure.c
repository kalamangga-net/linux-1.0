/*
 * sound/configure.c	- Configuration program for the Linux Sound Driver
 * 
 * Copyright by Hannu Savolainen 1993
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer. 2.
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 */

#include <stdio.h>

#define B(x)	(1 << (x))

/*
 * Option numbers
 */

#define OPT_PAS		0
#define OPT_SB		1
#define OPT_ADLIB	2
#define OPT_LAST_MUTUAL	2

#define OPT_GUS		3
#define OPT_MPU401	4

#define OPT_HIGHLEVEL   5
#define OPT_SBPRO	5
#define OPT_SB16	6
#define OPT_AUDIO	7
#define OPT_MIDI_AUTO	8
#define OPT_MIDI	9
#define OPT_YM3812_AUTO	10	/* Select this automaticly if user selects
				 * MIDI or AdLib driver */
#define OPT_YM3812	11	/* Select this if the previous one was not
				 * selected */
#define OPT_SEQUENCER	12
#define OPT_CHIP_MIDI   13	/* New support added at UW - Milwauklee UW -
				 * Milwauklee */
#define OPT_LAST	12

#define ANY_DEVS (B(OPT_AUDIO)|B(OPT_MIDI)|B(OPT_SEQUENCER)|B(OPT_GUS)|B(OPT_MPU401))

typedef struct
  {
    unsigned long   conditions;
    unsigned long   exclusive_options;
    char            macro[20];
    int             verify;
    int             alias;
    int		    default_answ;
  }

hw_entry;


/*
 * The rule table for the driver options. The first field defines a set of
 * options which must be selected before this entry can be selected. The
 * second field is a set of options which are not allowed with this one. If
 * the fourth field is zero, the option is selected without asking
 * confirmation from the user.
 * 
 * With this version of the rule table it is possible to select just one type of
 * hardware.
 * 
 * NOTE!	Keep the following table and the questions array in sync with the
 * option numbering!
 */

hw_entry        hw_table[] =
{
/* 0 */
  {0, 0, "PAS", 1, 0, 0},
  {0, 0, "SB", 1, 0, 0},
  {0, B (OPT_PAS) | B (OPT_SB), "ADLIB", 1, 0, 0},

/* 3 */
  {0, 0, "GUS", 1, 0, 0},
  {0, 0, "MPU401", 1, 0, 0},
  {B (OPT_SB), B (OPT_PAS), "SBPRO", 1, 0, 1},
  {B (OPT_SB) | B (OPT_SBPRO), B (OPT_PAS), "SB16", 1, 0, 1},
  {B (OPT_SB) | B (OPT_PAS) | B (OPT_GUS), 0, "AUDIO", 1, 0, 1},
  {B (OPT_MPU401), 0, "MIDI_AUTO", 0, OPT_MIDI, 0},
  {B (OPT_SB) | B (OPT_PAS) | B (OPT_MPU401) | B (OPT_GUS), 0, "MIDI", 1, 0, 1},
  {B (OPT_ADLIB), 0, "YM3812_AUTO", 0, OPT_YM3812, 0},
  {B (OPT_SB) | B (OPT_PAS) | B (OPT_ADLIB), B (OPT_YM3812_AUTO), "YM3812", 1, 0, 1},
/* 10 */
  {B (OPT_MIDI) | B (OPT_YM3812) | B (OPT_YM3812_AUTO) | B (OPT_GUS), 0, "SEQUENCER", 0, 0, 1},
  {0, 0, "CHIP_MIDI", 1, 0, 0}
};

char           *questions[] =
{
  "ProAudioSpectrum 16 support",
  "SoundBlaster support",
  "AdLib support",
  "Gravis Ultrasound support",
  "MPU-401 support (NOT for SB16)",

  "SoundBlaster Pro support",
  "SoundBlaster 16 support",
  "digitized voice support",
  "This should not be asked",
  "MIDI interface support",
  "This should not be asked",
  "FM synthesizer (YM3812/OPL-3) support",
  "/dev/sequencer support",
  "MIDI on CHIP support"
};

unsigned long   selected_options = 0;
int sb_dma = 0;

int
can_select_option (int nr)
{
  switch (nr)
    {
    case 0:
      fprintf (stderr, "The SoundBlaster, AdLib and ProAudioSpectrum\n"
	       "cards cannot be installed at the same time\n");
      fprintf (stderr, "\nSelect at most one of them:\n");
      fprintf (stderr, "	- ProAudioSpectrum 16\n");
      fprintf (stderr, "	- SoundBlaster / SB Pro\n");
      fprintf (stderr, "          (Could be selected with PAS16 also\n"
	       "	  since there is a SB emulation on it)\n");
      fprintf (stderr, "	- AdLib\n");
      fprintf (stderr, "\nDon't enable SoundBlaster if you have GUS at 0x220!\n\n");
      break;

    case OPT_LAST_MUTUAL + 1:
      fprintf (stderr, "\nThe following cards should work with any other cards.\n"
	       "CAUTION! Don't enable MPU-401 if you don't have it.\n");
      break;

    case OPT_HIGHLEVEL:
      fprintf (stderr, "\nSelect one or more of the following options\n");
      break;


    }

  if (hw_table[nr].conditions)
    if (!(hw_table[nr].conditions & selected_options))
      return 0;

  if (hw_table[nr].exclusive_options)
    if (hw_table[nr].exclusive_options & selected_options)
      return 0;

  return 1;
}

int
think_positively (int def_answ)
{
  char            answ[512];
  int             len;

  if ((len = read (0, &answ, sizeof (answ))) < 1)
    {
      fprintf (stderr, "\n\nERROR! Cannot read stdin\n");

      perror ("stdin");
      printf ("#undef CONFIGURE_SOUNDCARD\n");
      printf ("#undef KERNEL_SOUNDCARD\n");
      exit (-1);
    }

  if (len < 2)			/* There is an additional LF at the end */
    return def_answ;

  answ[len - 1] = 0;

  if (!strcmp (answ, "y") || !strcmp (answ, "Y"))
    return 1;

  return 0;
}

int
ask_value (char *format, int default_answer)
{
  char            answ[512];
  int             len, num;

play_it_again_Sam:

  if ((len = read (0, &answ, sizeof (answ))) < 1)
    {
      fprintf (stderr, "\n\nERROR! Cannot read stdin\n");

      perror ("stdin");
      printf ("#undef CONFIGURE_SOUNDCARD\n");
      printf ("#undef KERNEL_SOUNDCARD\n");
      exit (-1);
    }

  if (len < 2)			/* There is an additional LF at the end */
    return default_answer;

  answ[len - 1] = 0;

  if (sscanf (answ, format, &num) != 1)
    {
      fprintf (stderr, "Illegal format. Try again: ");
      goto play_it_again_Sam;
    }

  return num;
}

int
main (int argc, char *argv[])
{
  int             i, num, def_size, full_driver = 1;
  char            answ[10];

  printf ("/*\tGenerated by configure. Don't edit!!!!\t*/\n\n");

  fprintf (stderr, "\nConfiguring the sound support\n\n");

  fprintf (stderr, "Do you want to include full version of the sound driver (n/y) ? ");

  if (think_positively (0))
    {
      selected_options = 0xffffffff & ~B (OPT_MPU401);
      fprintf (stderr, "Note! MPU-401 driver was not enabled\n");
      full_driver = 1;
    }
  else
    {
      fprintf (stderr, "Do you want to DISABLE the Sound Driver (n/y) ?");
      if (think_positively (0))
	{
	  printf ("#undef CONFIGURE_SOUNDCARD\n");
	  printf ("#undef KERNEL_SOUNDCARD\n");
	  exit (0);
	}
      /* Partial driver */

      full_driver = 0;

      for (i = 0; i <= OPT_LAST; i++)
	if (can_select_option (i))
	  {
	    if (!(selected_options & B (i)))	/* Not selected yet */
	      if (!hw_table[i].verify)
		{
		  if (hw_table[i].alias)
		    selected_options |= B (hw_table[i].alias);
		  else
		    selected_options |= B (i);
		}
	      else
		{
		  int def_answ = hw_table[i].default_answ;

		  fprintf (stderr, 
		     def_answ ? "  %s (y/n) ? " : "  %s (n/y) ? ", 
		     questions[i]);
		  if (think_positively (def_answ))
		    if (hw_table[i].alias)
		      selected_options |= B (hw_table[i].alias);
		    else
		      selected_options |= B (i);
		}
	  }
    }

  if (selected_options & B(OPT_SB16))
     selected_options |= B(OPT_SBPRO);

  if (!(selected_options & ANY_DEVS))
    {
      printf ("#undef CONFIGURE_SOUNDCARD\n");
      printf ("#undef KERNEL_SOUNDCARD\n");
      fprintf (stderr, "\n*** This combination is useless. Sound driver disabled!!! ***\n\n");
      exit (0);
    }
  else
    printf ("#define KERNEL_SOUNDCARD\n");

  for (i = 0; i <= OPT_LAST; i++)
    if (!hw_table[i].alias)
      if (selected_options & B (i))
	printf ("#undef  EXCLUDE_%s\n", hw_table[i].macro);
      else
	printf ("#define EXCLUDE_%s\n", hw_table[i].macro);


  printf ("#define EXCLUDE_PRO_MIDI\n");
  printf ("#define EXCLUDE_CHIP_MIDI\n");

  /*
   * IRQ and DMA settings
   */
  printf ("\n");

#if defined(linux)
  if (selected_options & B (OPT_SB) && selected_options & (B (OPT_AUDIO) | B (OPT_MIDI)))
    {
      fprintf (stderr, "\nIRQ number for SoundBlaster?\n"
	       "The IRQ adress is defined by the jumpers on your card and\n"
	       "7 is the factory default. Valid values are 9, 5, 7 and 10.\n"
	       "Enter the value: ");

      num = ask_value ("%d", 7);
      if (num != 9 && num != 5 && num != 7 && num != 10)
	{

	  fprintf (stderr, "*** Illegal input! ***\n");
	  num = 7;
	}
      fprintf (stderr, "SoundBlaster IRQ set to %d\n", num);
      printf ("#define SBC_IRQ %d\n", num);

      if (selected_options & B (OPT_SBPRO))
	{

	  fprintf (stderr, "\nDMA channel for SoundBlaster?\n"
		   "For SB 1.0, 1.5 and 2.0 this MUST be 1\n"
		   "SB Pro supports DMA channels 0, 1 and 3 (jumper)\n"
		   "For SB16 give the 8 bit DMA# here\n"
		   "The default value is 1\n"
		   "Enter the value: ");

	  num = ask_value ("%d", 1);
	  if (num < 0 || num > 3)
	    {

	      fprintf (stderr, "*** Illegal input! ***\n");
	      num = 1;
	    }
	  fprintf (stderr, "SoundBlaster DMA set to %d\n", num);
	  printf ("#define SBC_DMA %d\n", num);
	  sb_dma = num;
	}

      if (selected_options & B (OPT_SB16))
	{

	  fprintf (stderr, "\n16 bit DMA channel for SoundBlaster 16?\n"
	  	   "Possible values are 5, 6 or 7\n"
		   "The default value is 6\n"
		   "Enter the value: ");

	  num = ask_value ("%d", 6);
	  if ((num < 5 || num > 7) && (num != sb_dma))
	    {

	      fprintf (stderr, "*** Illegal input! ***\n");
	      num = 6;
	    }
	  fprintf (stderr, "SoundBlaster DMA set to %d\n", num);
	  printf ("#define SB16_DMA %d\n", num);

          fprintf (stderr, "\nI/O base for SB16 Midi?\n"
	       "Possible values are 300 and 330\n"
	       "The factory default is 330\n"
	       "Enter the SB16 Midi I/O base: ");

          num = ask_value ("%x", 0x330);
          fprintf (stderr, "SB16 Midi I/O base set to %03x\n", num);
          printf ("#define SB16MIDI_BASE 0x%03x\n", num);
	}
    }

  if (selected_options & B (OPT_PAS))
    {
      if (selected_options & (B (OPT_AUDIO) | B (OPT_MIDI)))
	{
	  fprintf (stderr, "\nIRQ number for ProAudioSpectrum?\n"
		   "The recommended value is the IRQ used under DOS.\n"
		   "Please refer to the ProAudioSpectrum User's Guide.\n"
		   "The default value is 10.\n"
		   "Enter the value: ");

	  num = ask_value ("%d", 10);
	  if (num == 6 || num < 3 || num > 15 || num == 2)	/* Illegal */
	    {

	      fprintf (stderr, "*** Illegal input! ***\n");
	      num = 10;
	    }
	  fprintf (stderr, "ProAudioSpectrum IRQ set to %d\n", num);
	  printf ("#define PAS_IRQ %d\n", num);
	}

      if (selected_options & B (OPT_AUDIO))
	{
	  fprintf (stderr, "\nDMA number for ProAudioSpectrum?\n"
		   "The recommended value is the DMA channel under DOS.\n"
		   "Please refer to the ProAudioSpectrum User's Guide.\n"
		   "The default value is 3\n"
		   "Enter the value: ");

	  num = ask_value ("%d", 3);
	  if (num == 4 || num < 0 || num > 7)
	    {

	      fprintf (stderr, "*** Illegal input! ***\n");
	      num = 3;
	    }
	  fprintf (stderr, "\nProAudioSpectrum DMA set to %d\n", num);
	  printf ("#define PAS_DMA %d\n", num);
	}
    }

  if (selected_options & B (OPT_GUS))
    {
      fprintf (stderr, "\nI/O base for Gravis Ultrasound?\n"
	       "Valid choises are 210, 220, 230, 240, 250 or 260\n"
	       "The factory default is 220\n"
	       "Enter the GUS I/O base: ");

      num = ask_value ("%x", 0x220);
      if ((num > 0x260) || ((num & 0xf0f) != 0x200) || ((num & 0x0f0) > 0x060))
	{

	  fprintf (stderr, "*** Illegal input! ***\n");
	  num = 0x220;
	}

      if ((selected_options & B (OPT_SB)) && (num == 0x220))
	{
	  fprintf (stderr, "FATAL ERROR!!!!!!!!!!!!!!\n"
		   "\t0x220 cannot be used if SoundBlaster is enabled.\n"
		   "\tRun the config again.\n");
	  printf ("#undef CONFIGURE_SOUNDCARD\n");
	  printf ("#undef KERNEL_SOUNDCARD\n");
	  exit (-1);
	}
      fprintf (stderr, "GUS I/O base set to %03x\n", num);
      printf ("#define GUS_BASE 0x%03x\n", num);

      fprintf (stderr, "\nIRQ number for Gravis UltraSound?\n"
	       "The recommended value is the IRQ used under DOS.\n"
	       "Please refer to the Gravis Ultrasound User's Guide.\n"
	       "The default value is 15.\n"
	       "Enter the value: ");

      num = ask_value ("%d", 15);
      if (num == 6 || num < 3 || num > 15 || num == 2)	/* Invalid */
	{

	  fprintf (stderr, "*** Illegal input! ***\n");
	  num = 15;
	}
      fprintf (stderr, "Gravis UltraSound IRQ set to %d\n", num);
      printf ("#define GUS_IRQ %d\n", num);

      fprintf (stderr, "\nDMA number for Gravis UltraSound?\n"
	       "The recommended value is the DMA channel under DOS.\n"
	       "Please refer to the Gravis Ultrasound User's Guide.\n"
	       "The default value is 6\n"
	       "Enter the value: ");

      num = ask_value ("%d", 6);
      if (num == 4 || num < 0 || num > 7)
	{
	  fprintf (stderr, "*** Illegal input! ***\n");
	  num = 6;
	}
      fprintf (stderr, "\nGravis UltraSound DMA set to %d\n", num);
      printf ("#define GUS_DMA %d\n", num);
    }

  if (selected_options & B (OPT_MPU401))
    {
      fprintf (stderr, "\nI/O base for MPU-401?\n"
	       "The factory default is 330\n"
	       "Enter the MPU-401 I/O base: ");

      num = ask_value ("%x", 0x330);
      fprintf (stderr, "MPU-401 I/O base set to %03x\n", num);
      printf ("#define MPU_BASE 0x%03x\n", num);

      fprintf (stderr, "\nIRQ number for MPU-401?\n"
	       "Valid numbers are: 3, 4, 5, 7 and 9(=2).\n"
	       "The default value is 5.\n"
	       "Enter the value: ");

      num = ask_value ("%d", 5);
      if (num == 6 || num < 3 || num > 15)	/* Used for floppy */
	{

	  fprintf (stderr, "*** Illegal input! ***\n");
	  num = 5;
	}
      fprintf (stderr, "MPU-401 IRQ set to %d\n", num);
      printf ("#define MPU_IRQ %d\n", num);
    }
#endif

  if (selected_options & B (OPT_AUDIO))
    {
      def_size = 16384;

      if (selected_options & (B (OPT_SBPRO) | B (OPT_PAS) | B(OPT_SB16)))
	def_size = 32768;

#ifndef __386BSD__
      if (((selected_options & B (OPT_PAS)) || (selected_options & B (OPT_SB16))) && 
          !full_driver)
	def_size = 65536;	/* PAS16 or SB16 */
#endif

      fprintf (stderr, "\nSelect the DMA buffer size (4096, 16384, 32768 or 65536 bytes)\n"
	       "%d is recommended value for this configuration.\n"
	       "Enter the value: ", def_size);

      num = ask_value ("%d", def_size);
      if (num != 4096 && num != 16384 && num != 32768 && num != 65536)
	{

	  fprintf (stderr, "*** Illegal input! ***\n");
	  num = def_size;
	}
      fprintf (stderr, "The DMA buffer size set to %d\n", num);
      printf ("#define DSP_BUFFSIZE %d\n", num);
    }

  printf ("#define SELECTED_SOUND_OPTIONS\t0x%08x\n", selected_options);
  fprintf (stderr, "The sound driver is now configured.\n");

#if defined(SCO) || defined(ISC) || defined(SYSV)
	fprintf(stderr, "Rember to update the System file\n");
#endif

  exit (0);
}
