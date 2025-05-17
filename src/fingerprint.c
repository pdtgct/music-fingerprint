/*
 *  fingerprint.c
 *  executable to fingerprint an mp3 file
 *
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <libavutil/common.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "fplib.h"

int main(int argc, const char *argv[])
{
  const char *usage_fmt =
      "Usage: %s [-h] INPUT[music file] [-v]\n"
      "fingerprint from an audio file and write to stdout\n\n"
      "  -v   optional, verbose: print metadata to stdout\n"
      "  -h   print this message\n";
  const char *filename = NULL;
  int errn;
  int verbose = 0;
  FPrint *fp = NULL;

  ffmpeg_init();

  if (argc < 2)
  {
    printf(usage_fmt, argv[0]);
    return ENOENT;
  }
  filename = argv[1];
  if (strncmp(filename, "-h", 2) == 0)
  {
    printf(usage_fmt, argv[0]);
    return 0;
  }

  if (argc > 2 && strncmp(filename, "-v", 2) == 0)
  {
    filename = argv[2];
    verbose = 1;
  }

  fp = get_fingerprint(filename, &errn, verbose);
  if (!fp || errn != 0)
  {
    return errn;
  }

  printf("fingerprint:\n"
         "songlen:    %u\n"
         "bit_rate:   %u\n"
         "num_errors: %u\n",
         fp->songlen,
         fp->bit_rate,
         fp->num_errors);
  printf("r:         ");
  uint8_t *rbuf = fp->r;
  for (int i = 0; i < R_SIZE; i++)
  {
    printf("%02X", rbuf[i]);
  }
  printf("\ndom:       ");
  uint8_t *dom_buf = fp->dom;
  for (int j = 0; j < DOM_SIZE; j++)
  {
    printf("%02X", dom_buf[j]);
  }
  printf("\ncprint:    ");
  int32_t *cp_buf = fp->cprint;
  size_t cp_len = fp->cprint_len;
  for (size_t k = 0; k < cp_len; k++)
  {
    printf("%d ", cp_buf[k]);
  }
  printf("\n");

  if (fp)
  {
    free_fprint(fp);
    fp = NULL;
  }

  return 0;
}
