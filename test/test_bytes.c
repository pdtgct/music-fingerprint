/*
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fplib.h"

#define MASSERT(expr, msg) \
  if (!(expr))             \
    printf(msg);

int main(int argc, const char *argv[])
{
  int err = 0;
  int verbose = 0;
  FPrint *f1 = NULL;
  FPrint *f2 = NULL;

  ffmpeg_init();

  f1 = get_fingerprint("blue.mp3", &err, verbose);
  if (!f1)
  {
    printf("error obtaining fingerprint\n");
    return 1;
  }

  PackedFP *pbytes = (PackedFP *)fprint_to_bytes(f1);
  if (!pbytes)
  {
    printf("error converting to bytes\n");
    if (f1)
      free_fprint(f1);
    return 1;
  }

  f2 = fprint_from_bytes((uint8_t *)pbytes);
  if (!f2)
  {
    printf("error converting from bytes\n");
    if (pbytes)
    {
      free(pbytes);
      pbytes = NULL;
    }
    if (f1)
      free_fprint(f1);
    return 1;
  }
  if (pbytes)
  {
    free(pbytes);
    pbytes = NULL;
  }

  MASSERT(f2->songlen == f1->songlen, "songlen does not match\n");
  MASSERT(f2->cprint_len == f1->cprint_len, "cprint_len does not match\n");
  MASSERT(f2->bit_rate == f1->bit_rate, "bit_rate does not match\n");
  MASSERT(f2->num_errors == f1->num_errors, "num_errors does not match\n");

  for (int i = 0; i < R_SIZE; i++)
  {
    if (f2->r[i] != f1->r[i])
    {
      printf("bad match in r: %x != %x, ix: %d\n", f2->r[i], f1->r[i], i);
      break;
    }
  }

  for (int j = 0; j < DOM_SIZE; j++)
  {
    if (f2->dom[j] != f1->dom[j])
    {
      printf("bad match in dom: %x != %x, ix: %d\n",
             f2->dom[j], f1->dom[j], j);
      break;
    }
  }

  for (size_t k = 0; k < f1->cprint_len; k++)
  {
    if (f2->cprint[k] != f1->cprint[k])
    {
      printf("bad match in cprint: %x != %x, ix: %lu\n",
             f2->cprint[k], f1->cprint[k], k);
      break;
    }
  }

  if (f2)
    free_fprint(f2);
  if (f1)
    free_fprint(f1);

  return 0;
}
