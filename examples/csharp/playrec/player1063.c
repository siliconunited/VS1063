/*

  VLSI Solution generic microcontroller example player / recorder for
  VS1063.

  v1.10 2016-05-09 HH  Added chip type recognition, modified quick sanity check
  v1.02 2012-11-28 HH  Untabified
  v1.01 2012-11-27 HH  Bug fixes
  v1.00 2012-11-23 HH  First release

*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "player.h"
/* Download the latest VS1063a Patches package and its vs1063a-patches.plg.
   The patches package is available at
   http://www.vlsi.fi/en/support/software/vs10xxpatches.html */
#include "vs1063a-patches.plg"

#define FILE_BUFFER_SIZE 512
#define SDI_MAX_TRANSFER_SIZE 32
#define SDI_END_FILL_BYTES_FLAC 12288
#define SDI_END_FILL_BYTES       2050
#define REC_BUFFER_SIZE 512


#define SPEED_SHIFT_CHANGE 128

/* How many transferred bytes between collecting data.
   A value between 1-8 KiB is typically a good value.
   If REPORT_ON_SCREEN is defined, a report is given on screen each time
   data is collected. */
#define REPORT_INTERVAL 4096
#if 1
#define REPORT_ON_SCREEN
#endif

/* Define PLAYER_USER_INTERFACE if you want to have a user interface in your
   player. */
#if 1
#define PLAYER_USER_INTERFACE
#endif

/* Define RECORDER_USER_INTERFACE if you want to have a user interface in your
   player. */
#if 1
#define RECORDER_USER_INTERFACE
#endif


#define min(a,b) (((a)<(b))?(a):(b))



enum AudioFormat {
  afUnknown,
  afRiff,
  afOggVorbis,
  afMp1,
  afMp2,
  afMp3,
  afAacMp4,
  afAacAdts,
  afAacAdif,
  afFlac,
  afWma,
} audioFormat = afUnknown;

const char *afName[] = {
  "unknown",
  "RIFF",
  "Ogg",
  "MP1",
  "MP2",
  "MP3",
  "AAC MP4",
  "AAC ADTS",
  "AAC ADIF",
  "FLAC",
  "WMA",
};


/*
  Read 32-bit increasing counter value from addr.
  Because the 32-bit value can change while reading it,
  read MSB's twice and decide which is the correct one.
*/
u_int32 ReadVS10xxMem32Counter(u_int16 addr) {
  u_int16 msbV1, lsb, msbV2;
  u_int32 res;

  WriteSci(SCI_WRAMADDR, addr+1);
  msbV1 = ReadSci(SCI_WRAM);
  WriteSci(SCI_WRAMADDR, addr);
  lsb = ReadSci(SCI_WRAM);
  msbV2 = ReadSci(SCI_WRAM);
  if (lsb < 0x8000U) {
    msbV1 = msbV2;
  }
  res = ((u_int32)msbV1 << 16) | lsb;
  
  return res;
}


/*
  Read 32-bit non-changing value from addr.
*/
u_int32 ReadVS10xxMem32(u_int16 addr) {
  u_int16 lsb;
  WriteSci(SCI_WRAMADDR, addr);
  lsb = ReadSci(SCI_WRAM);
  return lsb | ((u_int32)ReadSci(SCI_WRAM) << 16);
}


/*
  Read 16-bit value from addr.
*/
u_int16 ReadVS10xxMem(u_int16 addr) {
  WriteSci(SCI_WRAMADDR, addr);
  return ReadSci(SCI_WRAM);
}


/*
  Write 16-bit value to given VS10xx address
*/
void WriteVS10xxMem(u_int16 addr, u_int16 data) {
  WriteSci(SCI_WRAMADDR, addr);
  WriteSci(SCI_WRAM, data);
}

/*
  Write 32-bit value to given VS10xx address
*/
void WriteVS10xxMem32(u_int16 addr, u_int32 data) {
  WriteSci(SCI_WRAMADDR, addr);
  WriteSci(SCI_WRAM, (u_int16)data);
  WriteSci(SCI_WRAM, (u_int16)(data>>16));
}






/*

  Loads a plugin.

  This is a slight modification of the LoadUserCode() example
  provided in many of VLSI Solution's program packages.

*/
void LoadPlugin(const u_int16 *d, u_int16 len) {
  int i = 0;

  while (i<len) {
    unsigned short addr, n, val;
    addr = d[i++];
    n = d[i++];
    if (n & 0x8000U) { /* RLE run, replicate n samples */
      n &= 0x7FFF;
      val = d[i++];
      while (n--) {
        WriteSci(addr, val);
      }
    } else {           /* Copy run, copy n samples */
      while (n--) {
        val = d[i++];
        WriteSci(addr, val);
      }
    }
  }
}









enum PlayerStates {
  psPlayback = 0,
  psUserRequestedCancel,
  psCancelSentToVS10xx,
  psStopped
} playerState;





/*

  This function plays back an audio file.

  It also contains a simple user interface, which requires the following
  funtions that you must provide:
  void SaveUIState(void);
  - saves the user interface state and sets the system up
  - may in many cases be implemented as an empty function
  void RestoreUIState(void);
  - Restores user interface state before exit
  - may in many cases be implemented as an empty function
  int GetUICommand(void);
  - Returns -1 for no operation
  - Returns -2 for cancel playback command
  - Returns any other for user input. For supported commands, see code.

*/
void VS1063PlayFile(FILE *readFp) {
  static u_int8 playBuf[FILE_BUFFER_SIZE];
  u_int32 bytesInBuffer;        // How many bytes in buffer left
  u_int32 pos=0;                // File position
  int endFillByte = 0;          // What byte value to send after file
  int endFillBytes = SDI_END_FILL_BYTES; // How many of those to send
  int playMode = ReadVS10xxMem(PAR_PLAY_MODE);
  static int vuMeter = 0;       // VU meter active
  long nextReportPos=0; // File pointer where to next collect/report
  int i;
#ifdef PLAYER_USER_INTERFACE
  static int earSpeaker = 0;    // 0 = off, other values strength
  static int speedShift = 16384;// 16384 = normal speed
  int volLevel = ReadSci(SCI_VOL) & 0xFF; // Assume both channels at same level
  int c;
  static int rateTune = 0;      // Samplerate fine tuning in ppm
#endif /* PLAYER_USER_INTERFACE */

#ifdef PLAYER_USER_INTERFACE
  SaveUIState();
#endif /* PLAYER_USER_INTERFACE */

  playerState = psPlayback;             // Set state to normal playback

  WriteSci(SCI_DECODE_TIME, 0);         // Reset DECODE_TIME


  /* Main playback loop */

  while ((bytesInBuffer = fread(playBuf, 1, FILE_BUFFER_SIZE, readFp)) > 0 &&
         playerState != psStopped) {
    u_int8 *bufP = playBuf;

    while (bytesInBuffer && playerState != psStopped) {

      if (!(playMode & PAR_PLAY_MODE_PAUSE_ENA)) {
        int t = min(SDI_MAX_TRANSFER_SIZE, bytesInBuffer);

        // This is the heart of the algorithm: on the following line
        // actual audio data gets sent to VS10xx.
        WriteSdi(bufP, t);

        bufP += t;
        bytesInBuffer -= t;
        pos += t;
      }

      /* If the user has requested cancel, set VS10xx SM_CANCEL bit */
      if (playerState == psUserRequestedCancel) {
        unsigned short oldMode;
        playerState = psCancelSentToVS10xx;
        printf("\nSetting SM_CANCEL at file offset %ld\n", pos);
        oldMode = ReadSci(SCI_MODE);
        WriteSci(SCI_MODE, oldMode | SM_CANCEL);
      }

      /* If VS10xx SM_CANCEL bit has been set, see if it has gone
         through. If it is, it is time to stop playback. */
      if (playerState == psCancelSentToVS10xx) {
        unsigned short mode = ReadSci(SCI_MODE);
        if (!(mode & SM_CANCEL)) {
          printf("SM_CANCEL has cleared at file offset %ld\n", pos);
          playerState = psStopped;
        }
      }


      /* If playback is going on as normal, see if we need to collect and
         possibly report */
      if (playerState == psPlayback && pos >= nextReportPos) {
#ifdef REPORT_ON_SCREEN
        u_int16 sampleRate;
        u_int16 hehtoBitsPerSec;
        u_int16 h1 = ReadSci(SCI_HDAT1);
#endif

        nextReportPos += REPORT_INTERVAL;
        /* It is important to collect endFillByte while still in normal
           playback. If we need to later cancel playback or run into any
           trouble with e.g. a broken file, we need to be able to repeatedly
           send this byte until the decoder has been able to exit. */
        endFillByte = ReadVS10xxMem(PAR_END_FILL_BYTE);

#ifdef REPORT_ON_SCREEN
        if (h1 == 0x7665) {
          audioFormat = afRiff;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if (h1 == 0x4154) {
          audioFormat = afAacAdts;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if (h1 == 0x4144) {
          audioFormat = afAacAdif;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if (h1 == 0x574d) {
          audioFormat = afWma;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if (h1 == 0x4f67) {
          audioFormat = afOggVorbis;
          endFillBytes = SDI_END_FILL_BYTES_FLAC;
        } else if (h1 == 0x664c) {
          audioFormat = afFlac;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if (h1 == 0x4d34) {
          audioFormat = afAacMp4;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if ((h1 & 0xFFE6) == 0xFFE2) {
          audioFormat = afMp3;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if ((h1 & 0xFFE6) == 0xFFE4) {
          audioFormat = afMp2;
          endFillBytes = SDI_END_FILL_BYTES;
        } else if ((h1 & 0xFFE6) == 0xFFE6) {
          audioFormat = afMp1;
          endFillBytes = SDI_END_FILL_BYTES;
        } else {
          audioFormat = afUnknown;
          endFillBytes = SDI_END_FILL_BYTES_FLAC;
        }

        sampleRate = ReadSci(SCI_AUDATA);
        hehtoBitsPerSec = ReadVS10xxMem(PAR_BITRATE_PER_100);

        printf("\r%ldKiB "
               "%1ds %1.1f"
               "kb/s %dHz %s %s"
               " %04x   ",
               pos/1024,
               ReadSci(SCI_DECODE_TIME),
               hehtoBitsPerSec * 0.1,
               sampleRate & 0xFFFE, (sampleRate & 1) ? "stereo" : "mono",
               afName[audioFormat], h1
               );
          
        if (vuMeter) {
          int vu, l, r;
          vu = ReadVS10xxMem(PAR_VU_METER);
          l = vu >> 8;
          r = vu & 0xFF;
          printf("%2d %2d ", l, r);
        }
        fflush(stdout);
#endif /* REPORT_ON_SCREEN */
      }
    } /* if (playerState == psPlayback && pos >= nextReportPos) */
  


    /* User interface. This can of course be completely removed and
       basic playback would still work. */

#ifdef PLAYER_USER_INTERFACE
    /* GetUICommand should return -1 for no command and -2 for CTRL-C */
    c = GetUICommand();
    switch (c) {

      /* Volume adjustment */
    case '-':
      if (volLevel < 255) {
        volLevel++;
        WriteSci(SCI_VOL, volLevel*0x101);
      }
      break;
    case '+':
      if (volLevel) {
        volLevel--;
        WriteSci(SCI_VOL, volLevel*0x101);
      }
      break;

      /* Speed shifter adjustment */
    case '*':
      speedShift = 16384;
      playMode &= ~PAR_PLAY_MODE_SPEED_SHIFTER_ENA;
      WriteVS10xxMem(PAR_PLAY_MODE, playMode);
      printf("\nSpeedShifter off\n");
      break;
    case ';':
      if (speedShift > 11141) {
        speedShift -= SPEED_SHIFT_CHANGE;
      }
      speedShift -= SPEED_SHIFT_CHANGE;
      /* Intentional fall-though */
    case ':':
      if (speedShift < 26869) {
        speedShift += SPEED_SHIFT_CHANGE;
        WriteVS10xxMem(PAR_SPEED_SHIFTER, speedShift);
          
        playMode |= PAR_PLAY_MODE_SPEED_SHIFTER_ENA;
        WriteVS10xxMem(PAR_PLAY_MODE, playMode);
      }
      printf("\nSpeedShift at %d (%5.3f)\n",
             speedShift, speedShift*(1.0/16384.0));
      break;

      /* Show some interesting registers */
    case '_':
      printf("\nvol %1.1fdB, MODE %04x, ST %04x, "
             "HDAT1 %04x HDAT0 %04x\n",
             -0.5*volLevel,
             ReadSci(SCI_MODE),
             ReadSci(SCI_STATUS),
             ReadSci(SCI_HDAT1),
             ReadSci(SCI_HDAT0));
      printf("  sampleCounter %lu",
             ReadVS10xxMem32Counter(PAR_SAMPLE_COUNTER));
      printf(", sdiFree %u", ReadVS10xxMem(PAR_SDI_FREE));
      printf(", audioFill %u", ReadVS10xxMem(PAR_AUDIO_FILL));
      printf("\n  positionMSec %lu",
             ReadVS10xxMem32Counter(PAR_POSITION_MSEC));
      printf(", config1 0x%04x", ReadVS10xxMem(PAR_CONFIG1));
      printf("\n");
      break;

      /* Adjust play speed between 1x - 4x */
    case '1':
    case '2':
    case '3':
    case '4':
      /* FF speed */
      printf("\nSet playspeed to %dX\n", c-'0');
      WriteVS10xxMem(PAR_PLAY_SPEED, c-'0');
      break;

      /* Ask player nicely to stop playing the song. */
    case 'q':
      if (playerState == psPlayback)
        playerState = psUserRequestedCancel;
      break;

      /* Forceful and ugly exit. For debug uses only. */
    case 'Q':
      RestoreUIState();
      printf("\n");
      exit(EXIT_SUCCESS);
      break;

      /* EarSpeaker spatial processing adjustment. */
    case 'e':
      earSpeaker = (earSpeaker+8192) & 0xFFFF;
      printf("\n");
      printf("Set earspeaker to %d\n", earSpeaker);
      WriteVS10xxMem(PAR_EARSPEAKER_LEVEL, earSpeaker);
      break;

      /* Toggle VU meter on/off */
    case 'u':
      vuMeter = 1-vuMeter;
      if (vuMeter) {
        playMode |= PAR_PLAY_MODE_VU_METER_ENA;
        printf("\nVU meter on\n");
      } else {
        playMode &= ~PAR_PLAY_MODE_VU_METER_ENA;
        printf("\nVU meter off\n");
      }
      WriteVS10xxMem(PAR_PLAY_MODE, playMode);
      break;

      /* Toggle pause mode */
    case 'p':
      playMode ^= PAR_PLAY_MODE_PAUSE_ENA;
      printf("\nPause mode %s\n",
             (playMode & PAR_PLAY_MODE_PAUSE_ENA) ? "on" : "off");
      WriteVS10xxMem(PAR_PLAY_MODE, playMode);
      break;

      /* Toggle mono mode */
    case 'm':
      playMode ^= PAR_PLAY_MODE_MONO_ENA;
      printf("\nMono mode %s\n",
             (playMode & PAR_PLAY_MODE_MONO_ENA) ? "on" : "off");
      WriteVS10xxMem(PAR_PLAY_MODE, playMode);
      break;

      /* Toggle differential mode */
    case 'd':
      {
        u_int16 t = ReadSci(SCI_MODE) ^ SM_DIFF;
        printf("\nDifferential mode %s\n", (t & SM_DIFF) ? "on" : "off");
        WriteSci(SCI_MODE, t);
      }
      break;

      /* Adjust playback samplerate finetuning */
    case 'r':
      if (rateTune >= 0) {
        rateTune = (rateTune*0.95);
      } else {
        rateTune = (rateTune*1.05);
      }
      rateTune -= 2;
      if (rateTune < -990000)
        rateTune = -990000;
      WriteVS10xxMem32(PAR_RATE_TUNE, rateTune);
      printf("\nrateTune %d ppm\n", rateTune);
      break;
    case 'R':
      if (rateTune <= 0) {
        rateTune = (rateTune*0.95);
      } else {
        rateTune = (rateTune*1.05);
      }
      rateTune += 2;
      WriteVS10xxMem32(PAR_RATE_TUNE, rateTune);
      printf("\nrateTune %d ppm\n", rateTune);
      break;
    case '/':
      rateTune = 0;
      WriteVS10xxMem32(PAR_RATE_TUNE, rateTune);
      printf("\nrateTune off\n");
      break;

      /* Show help */
    case '?':
      printf("\nInteractive VS1063 file player keys:\n"
             "1-4\tSet playback speed\n"
             "- +\tVolume down / up\n"
             "; :\tSpeedShift down / up\n"
             "*\tSpeedShift off\n"
             "_\tShow current settings\n"
             "q Q\tQuit current song / program\n"
             "e\tSet earspeaker\n"
             "t\tToggle temporary bitrate display\n"
             "r R\tR rateTune down / up\n"
             "/\tRateTune off\n"
             "u\tToggle VU Meter\n"
             "p\tToggle Pause\n"
             "m\tToggle Mono\n"
             "d\tToggle Differential\n"
             );
      break;

      /* Unknown commands or no command at all */
    default:
      if (c < -1) {
        printf("Ctrl-C, aborting\n");
        fflush(stdout);
        RestoreUIState();
        exit(EXIT_FAILURE);
      }
      if (c >= 0) {
        printf("\nUnknown char '%c' (%d)\n", isprint(c) ? c : '.', c);
      }
      break;
    } /* switch (c) */
#endif /* PLAYER_USER_INTERFACE */
  } /* while ((bytesInBuffer = fread(...)) > 0 && playerState != psStopped) */


  
#ifdef PLAYER_USER_INTERFACE
  RestoreUIState();
#endif /* PLAYER_USER_INTERFACE */

  printf("\nSending %d footer %d's... ", endFillBytes, endFillByte);
  fflush(stdout);

  /* Earlier we collected endFillByte. Now, just in case the file was
     broken, or if a cancel playback command has been given, write
     lots of endFillBytes. */
  memset(playBuf, endFillByte, sizeof(playBuf));
  for (i=0; i<endFillBytes; i+=SDI_MAX_TRANSFER_SIZE) {
    WriteSdi(playBuf, SDI_MAX_TRANSFER_SIZE);
  }

  /* If the file actually ended, and playback cancellation was not
     done earlier, do it now. */
  if (playerState == psPlayback) {
    unsigned short oldMode = ReadSci(SCI_MODE);
    WriteSci(SCI_MODE, oldMode | SM_CANCEL);
    printf("ok. Setting SM_CANCEL, waiting... ");
    fflush(stdout);
    while (ReadSci(SCI_MODE) & SM_CANCEL)
      WriteSdi(playBuf, 2);
  }

  /* That's it. Now we've played the file as we should, and left VS10xx
     in a stable state. It is now safe to call this function again for
     the next song, and again, and again... */
  printf("ok\n");
}













/*
  This function records an audio file in Ogg, MP3, or WAV formats.
  If recording in WAV format, it updates the RIFF length headers
  after recording has finished.
*/
void VS1063RecordFile(FILE *writeFp) {
  static u_int8 recBuf[REC_BUFFER_SIZE];
  u_int32 nextReportPos=0;      // File pointer where to next collect/report
  u_int32 fileSize = 0;
  int volLevel = ReadSci(SCI_VOL) & 0xFF;
  int c;

  playerState = psPlayback;

  printf("VS1063RecordFile\n");

  /* Initialize recording */

  /* This clock is high enough for both Ogg and MP3. */
  WriteSci(SCI_CLOCKF,
           HZ_TO_SC_FREQ(12288000) | SC_MULT_53_50X | SC_ADD_53_00X);
  /* The serial number field is used only by the Ogg Vorbis encoder,
     and even then only if told to used the field. If you use to
     encode Ogg Vorbis, use a randomizer or other function that creates
     a different serial number for each file. */
  WriteVS10xxMem32(PAR_ENC_SERIAL_NUMBER, 0x87654321);

#if 0
  /* Example definitions for MP3 recording.
     For best quality, record at 48 kHz.
     If you must use CBR, set bitrate to at least 160 kbit/s. Avoid 128 kbit/s.
     Preferably use VBR mode, which generally gives better results for a
     given bitrate. */
  WriteSci(SCI_RECRATE,     48000);
  WriteSci(SCI_RECGAIN,      1024); /* 1024 = gain 1 = best quality */
  WriteSci(SCI_RECMODE, RM_63_FORMAT_MP3 | RM_63_ADC_MODE_JOINT_AGC_STEREO);
  if (1) {
    /* Example of VBR mode */
    WriteSci(SCI_RECQUALITY, RQ_MODE_VBR | RQ_MULT_1000 | 160); /* ~160 kbps */
  } else {
    /* Example of CBR mode */
    WriteSci(SCI_RECQUALITY, RQ_MODE_CBR | RQ_MULT_1000 | 160); /*  160 kbps */
  }
  audioFormat = afMp3;
#elif 1
  /* Example definitions for Ogg Vorbis recording.
     For best quality, record at 48 kHz.
     The quality mode gives the best results. */
  WriteSci(SCI_RECRATE,     48000);
  WriteSci(SCI_RECGAIN,      1024); /* 1024 = gain 1 = best quality */
  WriteSci(SCI_RECMODE,
           RM_63_FORMAT_OGG_VORBIS | RM_63_ADC_MODE_JOINT_AGC_STEREO);
  WriteSci(SCI_RECQUALITY, RQ_MODE_QUALITY | RQ_OGG_PAR_SERIAL_NUMBER | 5);
  audioFormat = afOggVorbis;
#elif 1
  /* HiFi stereo quality PCM recording in stereo 48 kHz.
     This will result in a really fast 1536 kbit/s bitstream. Because
     there is a 100% overhead in reading from SCI, and because the data
     often has to be written to an SD card or similar using the same
     bus, the SPi speed must be really high and the software streamlined
     for there to be a chance for uninterrupted recording. */
  WriteSci(SCI_RECRATE,     48000);
  WriteSci(SCI_RECGAIN,      1024); /* 1024 = gain 1 = best quality */
  WriteSci(SCI_RECMODE, RM_63_FORMAT_PCM | RM_63_ADC_MODE_JOINT_AGC_STEREO);
  audioFormat = afRiff;
#else
  /* Example definitions for voice quality ADPCM recording from left channel
     at 8 kHz. This will result in a 33 kbit/s bitstream. */
  WriteSci(SCI_RECRATE,     8000);
  WriteSci(SCI_RECGAIN,        0); /* 1024 = gain 1 = best quality */
  WriteSci(SCI_RECMAXAUTO,  4096); /* if RECGAIN = 0, define max auto gain */
  WriteSci(SCI_RECMODE, RM_63_FORMAT_IMA_ADPCM | RM_63_ADC_MODE_LEFT);
  audioFormat = afRiff;
#endif

  WriteSci(SCI_MODE, ReadSci(SCI_MODE) | SM_LINE1 | SM_ENCODE);
  WriteSci(SCI_AIADDR, 0x0050); /* Activate recording! */


#ifdef RECORDER_USER_INTERFACE
  SaveUIState();
#endif /* RECORDER_USER_INTERFACE */

  while (playerState != psStopped) {
    int n;

#ifdef RECORDER_USER_INTERFACE
    {
      c = GetUICommand();
      
      switch(c) {
      case 'q':
        if (playerState == psPlayback) {
          WriteSci(SCI_MODE, ReadSci(SCI_MODE) | SM_CANCEL);
          printf("\nSwitching encoder off...\n");
          playerState = psUserRequestedCancel;
        }
        break;
      case '-':
        if (volLevel < 255) {
          volLevel++;
          WriteSci(SCI_VOL, volLevel*0x101);
        }
        break;
      case '+':
        if (volLevel) {
          volLevel--;
          WriteSci(SCI_VOL, volLevel*0x101);
        }
        break;
      case 'p':
        {
          int recMode = ReadSci(SCI_RECMODE) ^ RM_63_PAUSE;
          printf("\nPause mode %s\n", (recMode & RM_63_PAUSE) ? "on" : "off");
          WriteSci(SCI_RECMODE, recMode);
        }
        break;
      case '_':
        printf("\nvol %4.1f\n", -0.5*volLevel);
        break;
      case '?':
        printf("\nInteractive VS1063 file recorder keys:\n"
               "- +\tVolume down / up\n"
               "_\tShow current settings\n"
               "p\tToggle pause\n"
               "q\tQuit recording\n"
               );
        break;
      default:
        if (c < -1) {
          printf("Ctrl-C, aborting\n");
          fflush(stdout);
          RestoreUIState();
          exit(EXIT_FAILURE);
        }
        if (c >= 0) {
          printf("\nUnknown char '%c' (%d)\n", isprint(c) ? c : '.', c);
        }
        break;  
      }
      
    }
#endif /* RECORDER_USER_INTERFACE */


    /* See if there is some data available */
    if ((n = ReadSci(SCI_RECWORDS)) > 0) {
      int i;
      u_int8 *rbp = recBuf;

      n = min(n, REC_BUFFER_SIZE/2);
      for (i=0; i<n; i++) {
        u_int16 w = ReadSci(SCI_RECDATA);
        *rbp++ = (u_int8)(w >> 8);
        *rbp++ = (u_int8)(w & 0xFF);
      }
      fwrite(recBuf, 1, 2*n, writeFp);
      fileSize += 2*n;
    } else {
      /* The following read from SCI_RECWORDS may appear redundant.
         But it's not: SCI_RECWORDS needs to be rechecked AFTER we
         have seen that SM_CANCEL have cleared. */
      if (playerState != psPlayback && !(ReadSci(SCI_MODE) & SM_CANCEL)
          && !ReadSci(SCI_RECWORDS)) {
        playerState = psStopped;
      }
    }

    if (fileSize - nextReportPos >= REPORT_INTERVAL) {
      u_int16 sampleRate = ReadSci(SCI_AUDATA);
      nextReportPos += REPORT_INTERVAL;
      printf("\r%ldKiB %lds %uHz %s %s ",
             fileSize/1024,
             ReadVS10xxMem32Counter(PAR_SAMPLE_COUNTER) /
             (sampleRate & 0xFFFE),
             sampleRate & 0xFFFE,
             (sampleRate & 1) ? "stereo" : "mono",
             afName[audioFormat]
             );
      fflush(stdout);
    }
  } /* while (playerState != psStopped) */


#ifdef RECORDER_USER_INTERFACE
  RestoreUIState();
#endif /* RECORDER_USER_INTERFACE */

  /* We need to check whether the file had an odd length.
     That information is available in the MSB of PAR_END_FILL_BYTE.
     In that case, the 8 LSB's are the missing byte, so we'll add
     it to the output file. */
  {
    u_int16 lastByte;
    lastByte = ReadVS10xxMem(PAR_END_FILL_BYTE);
    if (lastByte & 0x8000U) {
      fputc(lastByte&0xFF, writeFp);
      printf("\nOdd length recording\n");
    } else {
      printf("\nEven length recording\n");
    } 
  }

  /* In case we were building a RIFF file (WAV PCM, WAV IMA ADPCM, etc),
     we need to correct the file length to the headers so that the file
     will be playable with all players. Unfortunately this requires
     seek and replace capabilities that are not necessarily available
     in all microcontroller environments. */
  if (audioFormat == afRiff) {
    unsigned long t;
    printf("\nCorrecting RIFF WAV headers\n");
    t = fileSize-8;
    fseek(writeFp, 4, SEEK_SET);
    fputc((t >>  0) & 0xFF, writeFp);
    fputc((t >>  8) & 0xFF, writeFp);
    fputc((t >> 16) & 0xFF, writeFp);
    fputc((t >> 24) & 0xFF, writeFp);
    t = fileSize-48;
    fseek(writeFp, 44, SEEK_SET);
    fputc((t >>  0) & 0xFF, writeFp);
    fputc((t >>  8) & 0xFF, writeFp);
    fputc((t >> 16) & 0xFF, writeFp);
    fputc((t >> 24) & 0xFF, writeFp);
  }


  /* Finally, reset the VS10xx software, including realoading the
     patches package, to make sure everything is set up properly. */
  VSTestInitSoftware();

  printf("ok\n");
}





/*

  Hardware Initialization for VS1063.

  
*/
int VSTestInitHardware(void) {
  /* Write here your microcontroller code which puts VS10xx in hardware
     reset anc back (set xRESET to 0 for at least a few clock cycles,
     then to 1). */
  return 0;
}



/* Note: code SS_VER=2 is used for both VS1002 and VS1011e */
const u_int16 chipNumber[16] = {
  1001, 1011, 1011, 1003, 1053, 1033, 1063, 1103,
  0, 0, 0, 0, 0, 0, 0, 0
};

/*

  Software Initialization for VS1063.

  Note that you need to check whether SM_SDISHARE should be set in
  your application or not.
  
*/
int VSTestInitSoftware(void) {
  u_int16 ssVer;

  /* Start initialization with a dummy read, which makes sure our
     microcontoller chips selects and everything are where they
     are supposed to be and that VS10xx's SCI bus is in a known state. */
  ReadSci(SCI_MODE);

  /* First real operation is a software reset. After the software
     reset we know what the status of the IC is. You need, depending
     on your application, either set or not set SM_SDISHARE. See the
     Datasheet for details. */
  WriteSci(SCI_MODE, SM_SDINEW|SM_SDISHARE|SM_TESTS|SM_RESET);

  /* A quick sanity check: write to two registers, then test if we
     get the same results. Note that if you use a too high SPI
     speed, the MSB is the most likely to fail when read again. */
  WriteSci(SCI_AICTRL1, 0xABAD);
  WriteSci(SCI_AICTRL2, 0x7E57);
  if (ReadSci(SCI_AICTRL1) != 0xABAD || ReadSci(SCI_AICTRL2) != 0x7E57) {
    printf("There is something wrong with VS10xx SCI registers\n");
    return 1;
  }
  WriteSci(SCI_AICTRL1, 0);
  WriteSci(SCI_AICTRL2, 0);

  /* Check VS10xx type */
  ssVer = ((ReadSci(SCI_STATUS) >> 4) & 15);
  if (chipNumber[ssVer]) {
    printf("Chip is VS%d\n", chipNumber[ssVer]);
    if (chipNumber[ssVer] != 1063) {
      printf("Incorrect chip\n");
      return 1;
    }
  } else {
    printf("Unknown VS10xx SCI_MODE field SS_VER = %d\n", ssVer);
    return 1;
  }

  /* Set the clock. Until this point we need to run SPI slow so that
     we do not exceed the maximum speeds mentioned in
     Chapter SPI Timing Diagram in the Datasheet. */
  WriteSci(SCI_CLOCKF,
           HZ_TO_SC_FREQ(12288000) | SC_MULT_53_40X | SC_ADD_53_15X);


  /* Now when we have upped the VS10xx clock speed, the microcontroller
     SPI bus can run faster. Do that before you start playing or
     recording files. */

  /* Set up other parameters. */
  WriteVS10xxMem(PAR_CONFIG1, PAR_CONFIG1_AAC_SBR_SELECTIVE_UPSAMPLE);

  /* Set volume level at -6 dB of maximum */
  WriteSci(SCI_VOL, 0x0c0c);

  /* Now it's time to load the proper patch set. */
  LoadPlugin(plugin, sizeof(plugin)/sizeof(plugin[0]));

  /* We're ready to go. */
  return 0;
}





/*
  Main function that activates either playback or recording.
*/
int VSTestHandleFile(const char *fileName, int record) {
  if (!record) {
    FILE *fp = fopen(fileName, "rb");
    printf("Play file %s\n", fileName);
    if (fp) {
      VS1063PlayFile(fp);
    } else {
      printf("Failed opening %s for reading\n", fileName);
      return -1;
    }
  } else {
    FILE *fp = fopen(fileName, "wb");
    printf("Record file %s\n", fileName);
    if (fp) {
      VS1063RecordFile(fp);
    } else {
      printf("Failed opening %s for writing\n", fileName);
      return -1;
    }
  }
  return 0;
}
