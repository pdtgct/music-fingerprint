/*
 *  fplib.c
 *  interface between FFMPEG, libfooid and chromaprint
 *  to extract a fingerprint from an audio file
 *
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
 *
 */

#include <errno.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <libavcodec/avcodec.h> /* includes ReSampleContext */
#include <libavutil/common.h>
#include <libavformat/avformat.h>
#include <libfooid/fooid.h>

#include "chromaw.h"
#include "fplib.h"

#if LIBAVCODEC_VERSION_MAJOR < 52
#error "This library requires ffmpeg version >= 53"
#endif

// audio resample constants
// standardized bitrate: 1 channel, 44100 Hz
#define STD_CHANNELS 1
#define STD_SAMPLE_RATE 44100

// record at most a minute of audio
#define SAMPLE_TIME_LIMIT 60

// R is scaled (max 25,056: 2x what reference (java) lib has)
#define MAX_RDIFF (9 * R_SIZE * CHAR_BIT)
// reference calculated max diff arithmetically
#define MAX_DOMDIFF (DOM_SIZE * CHAR_BIT)
#define MAX_TOTDIFF (MAX_RDIFF + MAX_DOMDIFF)
#define U32_BITS 32

#if defined(_64_BIT) || defined(__APPLE__)
#define ERROR_REALLOC_BUF "ERROR: unable to reallocate %s to %lu\n"
#define ERROR_ALLOC_CPRINT "ERROR: unable to allocate %lu bytes for cprint\n"
#else
#define ERROR_REALLOC_BUF "ERROR: unable to reallocate %s to %u\n"
#define ERROR_ALLOC_CPRINT "ERROR: unable to allocate %u bytes for cprint\n"
#endif

FPrint *new_fprint(int cprint_size)
{
  size_t cp_sz = 0;
  FPrint *p_fprint = NULL;

  if (cprint_size < 1)
  {
    cprint_size = 1;
  }
  else
  {
    cp_sz = (size_t)cprint_size;
  }

  p_fprint = calloc(1, CALC_FP_SIZE(cprint_size));
  if (!p_fprint)
  {
    return NULL;
  }
  p_fprint->cprint_len = cp_sz;

  return p_fprint;
}

void free_fprint(FPrint *fp)
{
  if (fp)
  {
    free(fp);
  }
}

void ffmpeg_init(void)
{
  avcodec_register_all();
  av_register_all();
}

FPrint *get_fingerprint(const char *filename, int *error, int verbose)
{
  int errn;
  AVFormatContext *ic = NULL;
  AVStream *st = NULL;
  int n_streams;
  AVCodecContext *cxt = NULL;
  AVCodec *dec_codec = NULL;
  ReSampleContext *resample = NULL;
  int n_samples = 0;
  int dec_sample_limit = 0;
  AVPacket pkt;
  int32_t len, dec_size, out_size;
  uint32_t last_size, min_size;
  int16_t *raw_buf = NULL;
  int16_t *audio_buf = NULL;
  int samplerate, channels;
  t_fooid *fid = NULL;
  uint8_t *fp_buf = NULL;
  float *fp_dbl_buf = NULL;
  int fp_size = 0;
  int ibps_sz = 0;
  int obps_sz = 0;
  FPrint *p_fprint = NULL;
  int32_t music_errors = 0;
  int fooid_stopped = 0;
  ChromaFingerprinter cpr = NULL;
  size_t cprint_len = 0;
  int32_t *cprint = NULL;

  // final NULL uses default parameters
  if ((errn = avformat_open_input(&ic, filename, NULL, NULL)) != 0 || !ic)
  {
    fprintf(stderr, "ERROR: %d: unable to open input file %s\n",
            errn, filename);
    fflush(stdout);
    *error = 1;
    goto cleanup;
  }

  if ((errn = avformat_find_stream_info(ic, NULL)) < 0)
  {
    fprintf(stderr, "ERROR: %d: unable to find format parameters\n", errn);
    fflush(stdout);
    *error = 1;
    goto cleanup;
  }

  // find audio stream (should be first and only one for music files)
  // AVCodecContext already initialized here
  n_streams = ic->nb_streams;
  for (int ads_ix = 0; ads_ix < n_streams; ads_ix++)
  {
    if (ic->streams[ads_ix]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      st = ic->streams[ads_ix];
      cxt = st->codec;
      break;
    }
  }
  if (!cxt)
  {
    fprintf(stderr, "ERROR: no audio stream found in file %s\n", filename);
    fflush(stdout);
    *error = 1;
    goto cleanup;
  }

  dec_codec = avcodec_find_decoder(cxt->codec_id);
  if (!dec_codec)
  {
    fprintf(stderr, "ERROR: no codec found for stream %s\n",
            cxt->codec_name);
    fflush(stdout);
    *error = 1;
    goto cleanup;
  }

  if ((errn = avcodec_open2(cxt, dec_codec, NULL)) < 0)
  {
    fprintf(stderr, "ERROR: unable to open dec_codec %s\n",
            cxt->codec_name);
    *error = errn;
    goto cleanup;
  }

  if (verbose)
    av_dump_format(ic, 0, filename, 0);

  // length (for VBR)
  // samples_per_frame / sample_rate * total_frames

  // already in Hz (samples / 1s)
  samplerate = cxt->sample_rate;
  channels = cxt->channels;
  dec_sample_limit = SAMPLE_TIME_LIMIT * samplerate * channels;

  // convert bps to sample size (uint8_t): >> 3 == / 8
  ibps_sz = av_get_bytes_per_sample(cxt->sample_fmt) >> 3;
  obps_sz = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16) >> 3;

  // clamp samples to 1 channel
  // this eliminates most sampling errors for chromaprint over bitrate
  // libfooid resamples to mono 64k but that is too reductive
  resample = av_audio_resample_init(STD_CHANNELS, channels,
                                    STD_SAMPLE_RATE, samplerate,
                                    AV_SAMPLE_FMT_S16, cxt->sample_fmt,
                                    16, 10, 0, 0.8);
  if (!resample)
  {
    fprintf(stderr,
            "ERROR: resample %d channels @ %d Hz to %d channels %d Hz\n",
            channels, samplerate, STD_CHANNELS, STD_SAMPLE_RATE);
    fflush(stderr);
    *error = errno == ENOMEM ? ENOMEM : 1;
    goto cleanup;
  }

  // libavcodec/avcodec.h
  // in uint8_t; audio frame size ~= 1s 48Khz 32bit audio
  // AVCODEC_MAX_AUDIO_FRAME_SIZE: 192,000
  // FF_INPUT_BUFFER_PADDING_SIZE: 8
  // FF_MIN_BUFFER_SIZE:           16,384
  last_size = 0;
  min_size = (AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2;
  // good alignment but unnecessary as malloc, calloc align 16:
  // min_size = FFMAX(17*min_size/16 + 32, min_size);
  raw_buf = (int16_t *)calloc(min_size, sizeof(*raw_buf));
  if (!raw_buf)
  {
    fprintf(stderr, "ERROR: unable to allocate raw_buf\n");
    fflush(stderr);
    *error = ENOMEM;
    goto cleanup;
  }
  last_size = min_size;
  audio_buf = (int16_t *)calloc(min_size, sizeof(*audio_buf));
  if (!audio_buf)
  {
    fprintf(stderr, "ERROR: unable to allocate audio_buf\n");
    fflush(stderr);
    *error = ENOMEM;
    goto cleanup;
  }
  fp_dbl_buf = (float *)calloc(min_size, sizeof(*fp_dbl_buf));
  if (!fp_dbl_buf)
  {
    fprintf(stderr, "ERROR: unable to allocate fp_dbl_buf\n");
    fflush(stderr);
    *error = ENOMEM;
    goto cleanup;
  }

  fid = fp_init(STD_SAMPLE_RATE, STD_CHANNELS);
  if (!fid)
  {
    fprintf(stderr, "ERROR: initializing fooid\n");
    fflush(stderr);
    *error = 1;
    goto cleanup;
  }

  cpr = chroma_init(STD_SAMPLE_RATE, STD_CHANNELS);
  if (!cpr)
  {
    fprintf(stderr, "ERROR: initializing chromaprint\n");
    fflush(stderr);
    *error = 1;
    goto cleanup;
  }

  n_samples = 0;
  for (;;)
  {
    av_init_packet(&pkt);

    errn = av_read_frame(ic, &pkt);
    if (errn == AVERROR(EAGAIN))
    {
      av_free_packet(&pkt);
      music_errors += 1;
      continue;
    }
    else if (errn < 0)
    {
      // EOF: no more packets
      av_free_packet(&pkt);
      break;
    }

    if (pkt.stream_index >= ic->nb_streams)
    {
      av_free_packet(&pkt);
      continue;
    }

    while (pkt.size > 0)
    {
      dec_size = FFMAX(pkt.size + FF_INPUT_BUFFER_PADDING_SIZE, min_size);
      // rarely ever need this except in cases of bad files...
      if (last_size < dec_size)
      {
        printf("reallocating...\n");
        // TODO: current FFMPEG raw_buf is an AVFrame*
        raw_buf = (int16_t *)realloc((void *)raw_buf,
                                     dec_size * sizeof(*raw_buf));
        if (!raw_buf)
        {
          fprintf(stderr, ERROR_REALLOC_BUF, "raw_buf",
                  dec_size * sizeof(*raw_buf));
          fflush(stderr);
          if (pkt.size > 0)
            av_free_packet(&pkt);
          *error = ENOMEM;
          goto cleanup;
        }
        audio_buf = (int16_t *)realloc((void *)audio_buf,
                                       dec_size * sizeof(*audio_buf));
        if (!audio_buf)
        {
          fprintf(stderr, ERROR_REALLOC_BUF, "audio_buf",
                  dec_size * sizeof(*audio_buf));
          fflush(stderr);
          if (pkt.size > 0)
            av_free_packet(&pkt);
          *error = ENOMEM;
          goto cleanup;
        }
        fp_dbl_buf = (float *)realloc((void *)fp_dbl_buf,
                                      dec_size * sizeof(*fp_dbl_buf));
        if (!fp_dbl_buf)
        {
          fprintf(stderr, ERROR_REALLOC_BUF, "fp_dbl_buf",
                  dec_size * sizeof(*fp_dbl_buf));
          fflush(stderr);
          if (pkt.size > 0)
            av_free_packet(&pkt);
          *error = ENOMEM;
          goto cleanup;
        }
        last_size = dec_size;
      }
      memset((void *)raw_buf, 0, last_size * sizeof(*raw_buf));
      memset((void *)audio_buf, 0, last_size * sizeof(*audio_buf));
      memset((void *)fp_dbl_buf, 0, last_size * sizeof(*fp_dbl_buf));

      len = avcodec_decode_audio3(cxt, raw_buf, &dec_size, &pkt);

      if (len < 0)
      {
        // len == -1 corresponds to a missing header
        if (len != -1)
        {
          fprintf(stderr, "ERROR: %d while decoding\n", len);
          fflush(stderr);
        }
        music_errors += 1;
        if (pkt.size > 0)
          av_free_packet(&pkt);
        continue;
      }

      // TODO: still getting floating point exception here
      if (dec_size > 0)
      {
        out_size = audio_resample(resample, audio_buf, raw_buf,
                                  dec_size / (channels * ibps_sz));
        // out_size only == STD_CHANNELS if the input data is already
        // int32_t PCM (single frame per packet)
        out_size *= STD_CHANNELS * obps_sz;
        errn = chroma_feed(cpr, audio_buf, out_size);
        if (errn != 0)
        {
          fprintf(stderr, "ERROR: feeding data to chromaprint\n");
          fflush(stderr);
          *error = 1;
          goto cleanup;
        }
        if (!fooid_stopped)
        {
          // pulled from fp_feed_short so we do not need to allocate
          // a new buffer each run through the loop
          for (int32_t i = 0; i < out_size; i++)
          {
            fp_dbl_buf[i] = (float)audio_buf[i] / 32767.0f;
          }
          errn = fp_feed_float(fid, fp_dbl_buf, out_size);
          if (errn == 0)
          {
            fooid_stopped = 1;
          }
          else if (errn < 0)
          {
            fprintf(stderr, "ERROR: feeding data to fooid\n");
            fflush(stderr);
            if (pkt.size > 0)
              av_free_packet(&pkt);
            *error = 1;
            goto cleanup;
          }
        }
        n_samples += out_size;
        if (n_samples >= dec_sample_limit)
        {
          // cut out based on the number of samples
          if (pkt.size > 0)
            av_free_packet(&pkt);
          goto fgprint;
        }
      }

      pkt.data += len;
      pkt.size -= len;
    }

    if (pkt.size > 0)
      av_free_packet(&pkt);
  }

  // no need to flush stream at end since we are working from file not FIFO

fgprint:
  if (n_samples <= 0)
  {
    fprintf(stderr, "ERROR: no samples for fingerprint\n");
    fflush(stderr);
    *error = 1;
    goto cleanup;
  }

  fp_size = fp_getsize(fid);
  if (fp_size <= 0)
  {
    fprintf(stderr, "ERROR: %d getting size for fingerprint\n", fp_size);
    fflush(stderr);
    *error = 1;
    goto cleanup;
  }

  fp_buf = (uint8_t *)malloc(fp_size);
  if (!fp_buf)
  {
    fprintf(stderr, "ERROR: allocating %d bytes for fp_buf\n", fp_size);
    fflush(stderr);
    *error = 1;
    goto cleanup;
  }

  if ((errn = fp_calculate(fid, n_samples, fp_buf)) < 0)
  {
    fprintf(stderr, "ERROR: %d calculating fingerprint\n", errn);
    fflush(stderr);
    *error = 1;
    goto cleanup;
  }

  cprint_len = 0;
  cprint = chroma_calculate(cpr, &errn, &cprint_len);
  if (errn != 0)
  {
    fprintf(stderr, "ERROR: %d calculating chromaprint\n", errn);
    if (errn == ENOMEM)
    {
      *error = ENOMEM;
    }
    else
    {
      *error = 1;
    }
    goto cleanup;
  }
  p_fprint = new_fprint((int)cprint_len);
  if (!p_fprint)
  {
    *error = ENOMEM;
    goto cleanup;
  }
  // convert duration to seconds, truncated: fractions inconsequential
  // WARNING: due to an ffmpeg bug (skips VBR header in favor of VBRI),
  // this duration may be incorrect: double by number of channels, so
  // a 23-second song registers as 46 seconds with 2 channels.
  p_fprint->songlen = (uint32_t)((double)st->duration * av_q2d(st->time_base));
  p_fprint->cprint_len = cprint_len;
  if (cxt->bit_rate > 0)
  {
    // if bit_rate encoded, it is in kbps
    p_fprint->bit_rate = cxt->bit_rate / 1000;
  }
  else
  {
    // for flac files, we have to estimate based on filesize and
    // (float) duration -- perhaps duration should always be a float?
    p_fprint->bit_rate = (uint32_t)ceil(((double)avio_size(ic->pb) * 8) / ((double)st->duration * av_q2d(st->time_base)) / 1000.0);
  }
  p_fprint->num_errors = music_errors;
  memcpy(p_fprint->r, fid->fp.r, R_SIZE * sizeof(uint8_t));
  memcpy(p_fprint->dom, fid->fp.dom, DOM_SIZE * sizeof(uint8_t));
  memcpy(p_fprint->cprint, cprint, cprint_len * sizeof(*cprint));

  *error = 0;

cleanup:
  if (cprint)
    free(cprint);
  if (cpr)
    chroma_destroy(cpr);
  if (raw_buf)
    free(raw_buf);
  if (audio_buf)
    free(audio_buf);
  if (fp_dbl_buf)
    free(fp_dbl_buf);
  if (fid)
    fp_free(fid);
  if (resample)
    audio_resample_close(resample);
  if (cxt)
    avcodec_close(cxt);
  if (ic)
    avformat_close_input(&ic);

  return p_fprint;
}

static inline uint32_t pop32(uint32_t x)
{
  x = x - ((x >> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
  return (((x + (x >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

static inline uint32_t pop16(uint16_t x)
{
  x = x - ((x >> 1) & 0x5555);
  x = (x & 0x3333) + ((x >> 2) & 0x3333);
  return (((x + (x >> 4)) & 0x0F0F) * 0x0101) >> 8;
}

static inline void rdiff_fooid32(uint32_t x, uint32_t *restrict rdiff)
{
  rdiff[(x & 0x3)]++;
  for (uint32_t i = 1; i < 16; i++)
  {
    rdiff[((x >> (i << 1)) & 0x3)]++;
  }
}

uint32_t hdist_r(const uint8_t *restrict r_a, const uint8_t *restrict r_b)
{
  uint32_t rdiff[4] = {0, 0, 0, 0};
  const uint32_t *r_a32 = (const uint32_t *)r_a;
  const uint32_t *r_b32 = (const uint32_t *)r_b;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    rdiff_fooid32(r_a32[i] ^ r_b32[i], rdiff);
  }
  return rdiff[1] + rdiff[2] * 4 + rdiff[3] * 9;
}

uint32_t hdist_dom(const uint8_t *restrict dom_a,
                   const uint8_t *restrict dom_b)
{
  unsigned int dist = 0;
  const uint32_t *dom32_a = (const uint32_t *)dom_a;
  const uint32_t *dom32_b = (const uint32_t *)dom_b;

  for (size_t i = 0; i < DOM_LEN32; i++)
  {
    dist += pop32(dom32_a[i] ^ dom32_b[i]);
  }
  dist += pop16(((uint16_t *)dom_a)[DOM_END16] ^ ((uint16_t *)dom_b)[DOM_END16]);

  return dist;
}

double match_fooid_fp(const uint8_t *restrict r_a,
                      const uint8_t *restrict dom_a,
                      const uint8_t *restrict r_b,
                      const uint8_t *restrict dom_b)
{
  const double maxdiff = (double)MAX_TOTDIFF;
  uint32_t diff_r = 0;
  uint32_t diff_dom = 0;
  double perc = 0.0;
  double conf = 0.0;
  uint32_t rdiff[4] = {0, 0, 0, 0};

  // scaled popcount for r (slow!)
  const uint32_t *r_a32 = (uint32_t *)r_a;
  const uint32_t *r_b32 = (uint32_t *)r_b;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    rdiff_fooid32(r_a32[i] ^ r_b32[i], rdiff);
  }
  diff_r = rdiff[1] + rdiff[2] * 4 + rdiff[3] * 9;

  // popcount for dom
  const uint32_t *dom32_a = (const uint32_t *)dom_a;
  const uint32_t *dom32_b = (const uint32_t *)dom_b;
  for (size_t i = 0; i < DOM_LEN32; i += 2)
  {
    diff_dom += pop32(dom32_a[i] ^ dom32_b[i]);
    diff_dom += pop32(dom32_a[i + 1] ^ dom32_b[i + 1]);
  }
  diff_dom += pop16(((uint16_t *)dom_a)[DOM_END16] ^ ((uint16_t *)dom_b)[DOM_END16]);

  // below is pretty much verbatim from the reference
  perc = (double)(diff_r + diff_dom) / maxdiff;
  conf = ((1.0 - perc) - 0.5) * 2.0;

  return fmax(fmin(conf, 1.0), 0.0);
}

#define POPCOUNT(x) __builtin_popcount((x))
#define ACOUSTID_MAX_ALIGN_OFFSET 120
#define ACOUSTID_MAX_BIT_ERROR 2

#define SWAP_I32_PTR(ptr1, ptr2) \
  {                              \
    const int32_t *tmp;          \
                                 \
    tmp = (ptr1);                \
    (ptr1) = (ptr2);             \
    (ptr2) = tmp;                \
  }
#define SWAP_NUMS(x, y) \
  {                     \
    (x) ^= (y);         \
    (y) ^= (x);         \
    (x) ^= (y);         \
  }

double match_chroma(const int32_t *restrict cp1, size_t cp1_len,
                    const int32_t *restrict cp2, size_t cp2_len,
                    size_t start, size_t end,
                    int *error)
{
  size_t i, j;
  size_t topcount = 0;
  size_t maxsize = cp1_len;
  size_t numcounts = cp1_len + cp2_len + 1;
  size_t jbegin, jend;
  size_t biterror = 0;
  size_t *counts = NULL;

  if (cp2_len > cp1_len)
  {
    maxsize = cp2_len;
    SWAP_NUMS(cp1_len, cp2_len)
    SWAP_I32_PTR(cp1, cp2)
  }
  // seek into stream
  if (start > 0 && end > start)
  {
    cp1_len = min_st(cp1_len, end);
    cp2_len = min_st(cp2_len, end);
    numcounts = cp1_len + cp2_len + 1;
    maxsize = cp2_len;
  }
  else
  {
    start = 0;
  }

  counts = (size_t *)calloc(numcounts, sizeof(*counts));
  if (!counts)
  {
    *error = ENOMEM;
    return 0.0;
  }

  for (i = start; i < cp1_len; i++)
  {
    jbegin = max_st(i - ACOUSTID_MAX_ALIGN_OFFSET, start);
    jend = min_st(i + ACOUSTID_MAX_ALIGN_OFFSET, cp2_len);
    for (j = jbegin; j < jend; j++)
    {
      biterror = POPCOUNT(cp1[i] ^ cp2[j]);
      if (biterror <= ACOUSTID_MAX_BIT_ERROR)
      {
        counts[i - j + cp2_len]++;
      }
    }
  }

  for (i = 0; i < numcounts; i++)
  {
    if (counts[i] > topcount)
      topcount = counts[i];
  }

  if (counts)
    free(counts);

  return (double)topcount / (double)(cp2_len - start);
}

/* This returns the value of the bit position, not the position itself.
 * To retrieve the position itself, convert log_2:
 *
 *  r = x & (-x);
 *  r |= (r >> 1);
 *  r |= (r >> 2);
 *  r |= (r >> 4);
 *  r |= (r >> 8);
 *  r |= (r >> 16);
 *  return popcount(r >> 1);
 *
 * where a faster implementation than __builtin_popcount() is above.
 */
static inline uint32_t cmp_low_bit(uint32_t x, uint32_t y)
{
  return (x & (-x)) == (y & (-y));
}

double match_chromab(const int32_t *restrict cp1, size_t cp1_len,
                     const int32_t *restrict cp2, size_t cp2_len)
{
  size_t maxsize = min_st(cp1_len, cp2_len);
  // better than sample correlation but seems to be 40%;
  // slower than hamming!!  Damn comparisons.
  size_t end = maxsize;
  size_t rem = end % 4;
  end >>= 2;
  register uint32_t sm = 0;
  uint32_t *cp1_32 = (uint32_t *)cp1;
  uint32_t *cp2_32 = (uint32_t *)cp2;

  if (maxsize == 0)
    return 0.0;

  for (size_t i = 0; i < end; i++)
  {
    sm += cmp_low_bit(*cp1_32++, *cp2_32++);
    sm += cmp_low_bit(*cp1_32++, *cp2_32++);
    sm += cmp_low_bit(*cp1_32++, *cp2_32++);
    sm += cmp_low_bit(*cp1_32++, *cp2_32++);
  }
  while (rem-- > 0)
  {
    sm += cmp_low_bit(*cp1_32++, *cp2_32++);
  }

  if (sm == 0)
    return 0.0;

  return (double)sm / (double)max_st(cp1_len, cp2_len);
}

/* // old popcount method (fast but in range .86 - 1.0; not very accurate)
 *
 * double match_chromah(int32_t* cp1, size_t cp1_len,
 *                      int32_t* cp2, size_t cp2_len) {
 *   size_t maxsize = min_st(cp1_len, cp2_len);
 *   size_t end = maxsize;
 *   size_t rem = end % 4;
 *   end >>= 2;
 *   uint64_t tot = maxsize * U32_BITS;
 *   uint64_t sm = 0;
 *   // avoid arithmetic shifts
 *   uint32_t* cp1_32 = (uint32_t*)cp1;
 *   uint32_t* cp2_32 = (uint32_t*)cp2;
 *   uint32_t a, b, c, d, e;
 *
 *   if (maxsize == 0) return 0.0;
 *
 *   for (size_t i = 0; i < end; i += 4) {
 *       a = pop32(*cp1_32++ ^ *cp2_32++);
 *       b = pop32(*cp1_32++ ^ *cp2_32++);
 *       c = pop32(*cp1_32++ ^ *cp2_32++);
 *       d = pop32(*cp1_32++ ^ *cp2_32++);
 *       sm += a + b + c + d;
 *   }
 *   while (rem-- > 0) {
 *       sm += pop32(*cp1_32++ ^ *cp2_32++);
 *   }
 *
 *   if (sm == 0) return 1.0;
 *   return  1.0 - (double)sm / (double)tot;
 */

/*  Note on Tanimoto (possible solution since bits are sparse):
 *  Tanimoto: popcount(a & b) / popcount(a | b)
 *
 *  This is close between hamming and bitpos but less reliable than chromab
 */
double match_chromat(const int32_t *restrict cp1, size_t cp1_len,
                     const int32_t *restrict cp2, size_t cp2_len)
{
  size_t maxsize = min_st(cp1_len, cp2_len);
  size_t end = maxsize;
  size_t rem = end % 4;
  end >>= 2;
  uint64_t tcomm = 0;
  uint64_t tdiff = 0;
  // avoid arithmetic shifts
  uint32_t *cp1_32 = (uint32_t *)cp1;
  uint32_t *cp2_32 = (uint32_t *)cp2;
  uint32_t a, b, c, d;

  if (maxsize == 0)
    return 0.0;

  for (size_t i = 0; i < end; i += 4)
  {
    a = pop32(cp1_32[i] & cp2_32[i]);
    b = pop32(cp1_32[i + 1] & cp2_32[i + 1]);
    c = pop32(cp1_32[i + 2] & cp2_32[i + 2]);
    d = pop32(cp1_32[i + 3] & cp2_32[i + 3]);
    tdiff += a + b + c + d;
    a = pop32(*cp1_32++ | *cp2_32++);
    b = pop32(*cp1_32++ | *cp2_32++);
    c = pop32(*cp1_32++ | *cp2_32++);
    d = pop32(*cp1_32++ | *cp2_32++);
    tcomm += a + b + c + d;
  }
  while (rem-- > 0)
  {
    tdiff = pop32(*cp1_32 & *cp2_32);
    tcomm += pop32(*cp1_32++ | *cp2_32++);
  }

  if (tdiff == 0)
    return 1.0;
  if (tcomm == 0)
    return 0.0;
  return (double)tdiff / (double)tcomm;
}

// calculate correlation coefficient
double match_chromac(const int32_t *restrict cp1, size_t cp1_len,
                     const int32_t *restrict cp2, size_t cp2_len)
{
  size_t maxsize = min_st(cp1_len, cp2_len);
  double vx, vy, r, n;
  double sx = 0.0;
  double px2 = 0.0;
  double sy = 0.0;
  double py2 = 0.0;
  double pxy = 0.0;
  int32_t *cp1_t = (int32_t *)cp1;
  int32_t *cp2_t = (int32_t *)cp2;

  for (size_t i = 0; i < maxsize; i++)
  {
    vx = (double)*cp1_t++;
    vy = (double)*cp2_t++;
    sx += vx;
    sy += vy;
    pxy += vx * vy;
    px2 += vx * vx;
    py2 += vy * vy;
  }
  n = (double)maxsize;
  r = (n * pxy - sx * sy) / (sqrt(n * px2 - (sx * sx)) * sqrt(n * py2 - (sy * sy)));
  // we are interested in the comparison, not slope
  return fabs(r);
}

double match_cpfm(FPrint *restrict a, FPrint *restrict b)
{
  if (!(a && b))
    return 0.0;

  float sl_a = (float)a->songlen;
  float sl_b = (float)b->songlen;
  float songlen_diff = fabsf(sl_a - sl_b);
  if (songlen_diff > (0.1f * fmin(sl_a, sl_b)))
  {
    return 0.0;
  }

  double fm = match_fooid_fp(a->r, a->dom, b->r, b->dom);
  double cp = match_chromab(a->cprint, a->cprint_len, b->cprint, b->cprint_len);

  return ((0.012985 + .263439 * fm + -.683234 * cp + 1.592623 * pow(cp, 3)) + 0.06348) / 1.2489;
}

void fprint_merge(FPrintUnion *restrict u,
                  const FPrint *restrict a,
                  const FPrint *restrict b)
{
  size_t cp_len = min_st(a->cprint_len, b->cprint_len);

  uint32_t *restrict r_u = (uint32_t *)u->r;
  const uint32_t *restrict r_a = (uint32_t *)a->r;
  const uint32_t *restrict r_b = (uint32_t *)b->r;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    r_u[i] = r_a[i] | r_b[i];
  }

  uint8_t *restrict dom_u = u->dom;
  const uint8_t *restrict dom_a = a->dom;
  const uint8_t *restrict dom_b = b->dom;
  uint32_t *restrict dom32_u = (uint32_t *)dom_u;
  const uint32_t *restrict dom32_a = (uint32_t *)dom_a;
  const uint32_t *restrict dom32_b = (uint32_t *)dom_b;
  for (size_t j = 0; j < DOM_LEN32; j++)
  {
    dom32_u[j] = dom32_a[j] | dom32_b[j];
  }

  ((uint16_t *restrict)dom_u)[DOM_END16] = ((const uint16_t *)dom_a)[DOM_END16] | ((const uint16_t *)dom_b)[DOM_END16];

  uint32_t *restrict cp_u = (uint32_t *)u->cprint;
  const uint32_t *restrict cp_a = (uint32_t *)a->cprint;
  const uint32_t *restrict cp_b = (uint32_t *)b->cprint;
  for (size_t l = 0; l < cp_len; l++)
  {
    cp_u[l] = cp_a[l] | cp_b[l];
  }
  if (a->cprint_len > cp_len)
  {
    for (size_t l = b->cprint_len; l < a->cprint_len; l++)
    {
      cp_u[l] |= cp_a[l];
    }
  }
  else if (b->cprint_len > cp_len)
  {
    for (size_t l = a->cprint_len; l < b->cprint_len; l++)
    {
      cp_u[l] |= cp_b[l];
    }
  }

  u->min_songlen = min_u32(a->songlen, b->songlen);
  u->max_songlen = max_u32(a->songlen, b->songlen);
}

void fprint_merge_one(FPrintUnion *restrict u, const FPrint *restrict a)
{
  size_t cp_len = a->cprint_len;

  uint32_t *restrict r_u = (uint32_t *)u->r;
  const uint32_t *restrict r_a = (uint32_t *)a->r;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    r_u[i] |= r_a[i];
  }

  uint8_t *restrict dom_u = u->dom;
  const uint8_t *restrict dom_a = a->dom;
  uint32_t *restrict dom32_u = (uint32_t *)dom_u;
  const uint32_t *restrict dom32_a = (uint32_t *)dom_a;
  for (size_t j = 0; j < DOM_LEN32; j++)
  {
    dom32_u[j] |= dom32_a[j];
  }
  ((uint16_t *restrict)dom_u)[DOM_END16] |= ((const uint16_t *)dom_a)[DOM_END16];

  uint32_t *restrict cp_u = (uint32_t *)u->cprint;
  const uint32_t *restrict cp_a = (uint32_t *)a->cprint;
  for (size_t l = 0; l < cp_len; l++)
  {
    cp_u[l] |= cp_a[l];
  }

  if (u->min_songlen > 0)
  {
    u->min_songlen = min_u32(u->min_songlen, a->songlen);
  }
  else
  {
    u->min_songlen = a->songlen;
  }
  u->max_songlen = max_u32(u->max_songlen, a->songlen);
}

void fprint_merge_one_union(FPrintUnion *restrict u, const FPrintUnion *restrict a)
{
  size_t cp_len = a->cprint_len;

  uint32_t *restrict r_u = (uint32_t *)u->r;
  const uint32_t *restrict r_a = (uint32_t *)a->r;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    r_u[i] |= r_a[i];
  }

  uint8_t *restrict dom_u = u->dom;
  const uint8_t *restrict dom_a = a->dom;
  uint32_t *restrict dom32_u = (uint32_t *)dom_u;
  const uint32_t *restrict dom32_a = (uint32_t *)dom_a;
  for (size_t j = 0; j < DOM_LEN32; j++)
  {
    dom32_u[j] |= dom32_a[j];
  }
  ((uint16_t *restrict)dom_u)[DOM_END16] |= ((const uint16_t *)dom_a)[DOM_END16];

  uint32_t *restrict cp_u = (uint32_t *)u->cprint;
  const uint32_t *restrict cp_a = (uint32_t *)a->cprint;
  for (size_t l = 0; l < cp_len; l++)
  {
    cp_u[l] |= cp_a[l];
  }

  if (u->min_songlen > 0)
  {
    u->min_songlen = min_u32(u->min_songlen, a->min_songlen);
  }
  else
  {
    u->min_songlen = a->min_songlen;
  }
  u->max_songlen = max_u32(u->max_songlen, a->max_songlen);
}

float match_fprint_merge(const FPrint *restrict a, const FPrintUnion *restrict u)
{
  const double maxdiff = (double)MAX_TOTDIFF;
  uint32_t diff_r = 0;
  uint32_t diff_dom = 0;
  float perc = 0.0f;
  float conf = 0.0f;
  float fooid = 0.0f;
  size_t cp_len = min_st(u->cprint_len, a->cprint_len);
  uint32_t diff_cp = 0;
  float chroma = 0.0f;

  uint32_t rdiff[4] = {0, 0, 0, 0};
  const uint32_t *restrict r_a = (const uint32_t *)a->r;
  const uint32_t *restrict r_u = (const uint32_t *)u->r;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    rdiff_fooid32(r_a[i] ^ (r_a[i] & r_u[i]), rdiff);
  }
  diff_r = rdiff[1] + rdiff[2] * 4 + rdiff[3] * 9;

  const uint8_t *restrict dom_a = a->dom;
  const uint8_t *restrict dom_u = u->dom;
  const uint32_t *restrict dom32_a = (const uint32_t *)dom_a;
  const uint32_t *restrict dom32_u = (const uint32_t *)dom_u;
  for (size_t j = 0; j < DOM_LEN32; j++)
  {
    diff_dom += pop32(dom32_a[j] ^ (dom32_a[j] & dom32_u[j]));
  }
  uint16_t a_d16 = ((uint16_t *)dom_a)[DOM_END16];
  diff_dom += pop16(a_d16 ^ (a_d16 & ((uint16_t *)dom_u)[DOM_END16]));

  perc = (float)(diff_r + diff_dom) / maxdiff;
  conf = ((1.0 - perc) - 0.5) * 2.0;
  fooid = fmaxf(fminf(conf, 1.0), 0.0);

  const uint32_t *restrict cp_a = (const uint32_t *)a->cprint;
  const uint32_t *restrict cp_u = (const uint32_t *)u->cprint;
  uint32_t x, y;
  for (size_t k = 0; k < cp_len; k++)
  {
    x = cp_a[k];
    y = cp_u[k];
    diff_cp += ((x == (x & y)) || cmp_low_bit(x, y));
  }

  if (cp_len > 0)
  {
    chroma = (float)diff_cp / (float)a->cprint_len;
  }

  float comb = ((0.012985 + .263439 * fooid + -.683234 * chroma + 1.592623 * (chroma * chroma * chroma)) + 0.06348) / 1.2489;

  return fmaxf(fminf(comb, 1.0), 0.0);
}

float match_merges(const FPrintUnion *restrict u1, const FPrintUnion *restrict u2)
{
  const double maxdiff = (double)MAX_TOTDIFF;
  uint32_t diff_r = 0;
  uint32_t diff_dom = 0;
  float perc = 0.0f;
  float conf = 0.0f;
  float fooid = 0.0f;
  size_t cp_len = min_st(u1->cprint_len, u2->cprint_len);
  uint32_t diff_cp = 0;
  float chroma = 0.0f;

  if (u1->max_songlen < u2->min_songlen || u2->max_songlen < u1->min_songlen)
    return 0.0f;

  uint32_t rdiff[4] = {0, 0, 0, 0};
  const uint32_t *restrict r_u1 = (const uint32_t *)u1->r;
  const uint32_t *restrict r_u2 = (const uint32_t *)u2->r;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    rdiff_fooid32(r_u1[i] ^ (r_u1[i] & r_u2[i]), rdiff);
  }
  diff_r = rdiff[1] + rdiff[2] * 4 + rdiff[3] * 9;

  const uint8_t *restrict dom_u1 = u1->dom;
  const uint8_t *restrict dom_u2 = u2->dom;
  const uint32_t *restrict dom32_u1 = (const uint32_t *)dom_u1;
  const uint32_t *restrict dom32_u2 = (const uint32_t *)dom_u2;
  for (size_t j = 0; j < DOM_LEN32; j++)
  {
    diff_dom += pop32(dom32_u1[j] ^ (dom32_u1[j] & dom32_u2[j]));
  }
  uint16_t u1_d16 = ((uint16_t *)dom_u1)[DOM_END16];
  diff_dom += pop16(u1_d16 ^ (u1_d16 & ((uint16_t *)dom_u2)[DOM_END16]));

  perc = (float)(diff_r + diff_dom) / maxdiff;
  conf = ((1.0 - perc) - 0.5) * 2.0;
  fooid = fmaxf(fminf(conf, 1.0), 0.0);

  const uint32_t *restrict cp_u1 = (const uint32_t *)u1->cprint;
  const uint32_t *restrict cp_u2 = (const uint32_t *)u2->cprint;
  uint32_t x, y;
  for (size_t k = 0; k < cp_len; k++)
  {
    x = cp_u1[k];
    y = cp_u2[k];
    diff_cp += ((x == (x & y)) || cmp_low_bit(x, y));
  }

  if (cp_len > 0)
  {
    chroma = (float)diff_cp / (float)u1->cprint_len;
  }

  float comb = ((0.012985 + .263439 * fooid + -.683234 * chroma + 1.592623 * (chroma * chroma * chroma)) + 0.06348) / 1.2489;

  return fmaxf(fminf(comb, 1.0), 0.0);
}

float try_match_merges(const FPrintUnion *restrict u1,
                       const FPrintUnion *restrict u2,
                       const FPrint *restrict a)
{
  const double maxdiff = (double)MAX_TOTDIFF;
  uint32_t diff_r = 0;
  uint32_t diff_dom = 0;
  float perc = 0.0f;
  float conf = 0.0f;
  float fooid = 0.0f;
  uint32_t x, y;
  uint32_t diff_cp = 0;
  float chroma = 0.0f;

  uint32_t rdiff[4] = {0, 0, 0, 0};
  const uint32_t *restrict r_u1 = (const uint32_t *)u1->r;
  const uint32_t *restrict r_u2 = (const uint32_t *)u2->r;
  const uint32_t *restrict r_a = (const uint32_t *)a->r;
  for (size_t i = 0; i < R_SIZE32; i++)
  {
    x = r_u1[i];
    y = (r_u2[i] | r_a[i]);
    rdiff_fooid32(x ^ (x & y), rdiff);
  }
  diff_r = rdiff[1] + rdiff[2] * 4 + rdiff[3] * 9;

  const uint8_t *restrict dom_u1 = u1->dom;
  const uint8_t *restrict dom_u2 = u2->dom;
  const uint8_t *restrict dom_a = a->dom;
  const uint32_t *restrict dom32_u1 = (const uint32_t *)dom_u1;
  const uint32_t *restrict dom32_u2 = (const uint32_t *)dom_u2;
  const uint32_t *restrict dom32_a = (const uint32_t *)dom_a;
  for (size_t j = 0; j < DOM_LEN32; j++)
  {
    x = dom32_u1[j];
    y = (dom32_u2[j] | dom32_a[j]);
    diff_dom += pop32(x ^ (x & y));
  }
  uint16_t z1 = ((const uint16_t *)dom_u1)[DOM_END16];
  uint16_t z2 = ((const uint16_t *)dom_u2)[DOM_END16] | ((const uint16_t *)dom_a)[DOM_END16];
  diff_dom += pop16(z1 ^ (z1 & z2));

  perc = (double)(diff_r + diff_dom) / maxdiff;
  conf = ((1.0 - perc) - 0.5) * 2.0;
  fooid = fmax(fmin(conf, 1.0), 0.0);

  const uint32_t *restrict cp_u1 = (const uint32_t *)u1->cprint;
  const uint32_t *restrict cp_u2 = (const uint32_t *)u2->cprint;
  const uint32_t *restrict cp_a = (const uint32_t *)a->cprint;
  size_t cp_len = min_st(min_st(u1->cprint_len, u2->cprint_len),
                         a->cprint_len);
  for (size_t k = 0; k < cp_len; k++)
  {
    x = cp_u1[k];
    y = (cp_u2[k] | cp_a[k]);
    diff_cp += ((x == (x & y)) || cmp_low_bit(x, y));
  }
  if (u1->cprint_len > cp_len)
  {
    if (a->cprint_len > cp_len)
    {
      cp_len = min_st(u1->cprint_len, a->cprint_len);
      for (size_t l = u2->cprint_len; l < cp_len; l++)
      {
        x = cp_u1[l];
        y = cp_a[l];
        diff_cp += ((x == (x & y)) || cmp_low_bit(x, y));
      }
    }
    else if (u2->cprint_len > cp_len)
    {
      cp_len = min_st(u1->cprint_len, u2->cprint_len);
      for (size_t l = a->cprint_len; l < cp_len; l++)
      {
        x = cp_u1[l];
        y = cp_u2[l];
        diff_cp += ((x == (x & y)) || cmp_low_bit(x, y));
      }
    }
  }

  if (cp_len > 0)
  {
    chroma = (float)diff_cp / (float)u1->cprint_len;
  }

  float comb = ((0.012985 + .263439 * fooid + -.683234 * chroma + 1.592623 * (chroma * chroma * chroma)) + 0.06348) / 1.2489;

  return fmaxf(fminf(comb, 1.0), 0.0);
}

// max 24 (10 each for songlen, bit_rate, num_errors) + 3 for "(,," + 1 '\0'
#define BASE_SIZE 24
#define BASEFMT "(%u,%u,%u,"

char *fprint_to_string(const FPrint *fp)
{
  char *tmpstr = NULL;
  int out_len = 0;
  char base[BASE_SIZE];
  size_t cprint_len = 0;
  const uint8_t *r = NULL;
  const uint8_t *dom = NULL;
  const int32_t *cprint = NULL;
  int base_sz = 0;
  size_t str_sz = 0;
  char *outstr = NULL;

  if (!fp)
  {
    tmpstr = (char *)calloc(1, sizeof(char));
    if (tmpstr)
      strncpy(tmpstr, "", 1);
    return tmpstr;
  }

  cprint_len = fp->cprint_len;
  r = fp->r;
  dom = fp->dom;
  cprint = fp->cprint;
  base_sz = snprintf(base, BASE_SIZE, BASEFMT,
                     fp->songlen, fp->bit_rate, fp->num_errors);
  // + 2 for following ")\0"; 9 to allow for '-'
  str_sz = base_sz + (2 * R_SIZE + 1) + (2 * DOM_SIZE + 1) + (12 * cprint_len) + 2;

  tmpstr = calloc(str_sz, sizeof(char));
  if (!tmpstr)
  {
    return NULL;
  }

  strncpy(tmpstr, base, str_sz);
  // chop terminating "\0"
  out_len += base_sz;

  for (int i = 0; i < R_SIZE; i++)
  {
    out_len += snprintf(&tmpstr[out_len], 3, "%02X", r[i]);
  }
  strncpy(&tmpstr[out_len++], ",", 2);

  for (int i = 0; i < DOM_SIZE; i++)
  {
    out_len += snprintf(&tmpstr[out_len], 3, "%02X", dom[i]);
  }
  strncpy(&tmpstr[out_len++], ",", 2);

  for (size_t j = 0; j < cprint_len; j++)
  {
    // 32-bit int max size is 10 chars + 2 ("-"? and " \0")
    out_len += snprintf(&tmpstr[out_len], 13, "%d ", cprint[j]);
  }
  // cheat: pull back from final " "
  out_len -= 1;
  strncpy(&tmpstr[out_len++], ")", 2);

  outstr = realloc(tmpstr, ((size_t)out_len + 1) * sizeof(*tmpstr));

  if (!outstr)
  {
    if (tmpstr)
      free(tmpstr);
    return NULL;
  }

  return outstr;
}

FPrint *fprint_from_string(const char *fp_str)
{
  FPrint *fp = NULL;
  uint32_t songlen = 0;
  uint32_t bit_rate = 0;
  uint32_t num_errors = 0;
  uint8_t r[R_SIZE];
  uint8_t dom[DOM_SIZE];
  int32_t *cprint = NULL;
  size_t cprint_len = 0;
  size_t cprint_ix = 0;
  char cpn_str[13];
  int cp_char_ix = 0;
  char c;
  int nret = 0;
  int n_commas = 0;
  int fp_str_len = 0;
  int fp_str_ix = 0;

  if (!fp_str)
    return NULL;

  //   7 for minimum: "(0,0,0,"
  // + 2 ",," after R and DOM
  // + 2 "0)" for minimum cprint and finishing ')'
  if ((fp_str_len = strlen(fp_str)) < (11 + 2 * R_SIZE + 2 * DOM_SIZE))
  {
    fprintf(stderr, "invalid string length: %d\n", fp_str_len);
    return NULL;
  }

  nret = sscanf(fp_str, BASEFMT, &songlen, &bit_rate, &num_errors);
  if (nret != 3)
  {
    fprintf(stderr, "missing one or more arguments at beginning of string\n");
    goto error;
  }

  n_commas = 0;
  while (n_commas < 3)
  {
    if (fp_str[fp_str_ix++] == ',')
      n_commas++;
  }

  for (int i = 0; i < R_SIZE; i += 4)
  {
    // this format spec is Posixly correct;
    // Darwin/BSD allows %hh02X as well
    nret = sscanf(&fp_str[fp_str_ix], "%2hhX%2hhX%2hhX%2hhX",
                  &r[i], &r[i + 1], &r[i + 2], &r[i + 3]);
    if (nret != 4)
    {
      fprintf(stderr, "invalid format for r block starting at character %d\n",
              fp_str_ix);
      goto error;
    }
    fp_str_ix += 8;
  }
  if (fp_str[fp_str_ix++] != ',')
  {
    fprintf(stderr, "missing ',' after r block\n");
    goto error;
  }

  for (int i = 0; i < DOM_SIZE; i += 2)
  {
    nret = sscanf(&fp_str[fp_str_ix], "%2hhX%2hhX", &dom[i], &dom[i + 1]);
    if (nret != 2)
    {
      fprintf(stderr, "invalid format for dom block starting at character %d\n",
              fp_str_ix);
      goto error;
    }
    fp_str_ix += 4;
  }
  if (fp_str[fp_str_ix++] != ',')
  {
    fprintf(stderr, "missing ',' after dom block\n");
    goto error;
  }

  cprint = (int32_t *)calloc(KNOWN_CPRINT_LEN, sizeof(*cprint));
  if (!cprint)
    goto error;
  cprint_len = KNOWN_CPRINT_LEN;

  c = fp_str[fp_str_ix++];
  while (c != '\0')
  {
    if (cp_char_ix >= 12)
    {
      fprintf(stderr, "integer ending at position %d is too wide\n",
              fp_str_ix - 1);
      goto error;
    }
    else if (c == ' ' || c == ')')
    {
      cpn_str[cp_char_ix] = '\0';
      cprint[cprint_ix++] = (int32_t)strtol(cpn_str, NULL, 10);
      cp_char_ix = 0;
      if (c == ')')
        break;
      if (cprint_ix >= cprint_len)
      {
        // we were off?  add another 30 seconds worth (15.8 ~= 1s)
        cprint_len += 474;
        int32_t *tmp = realloc(cprint, cprint_len * sizeof(*cprint));
        if (!tmp)
        {
          goto error;
        }
        cprint = tmp;
      }
      c = fp_str[fp_str_ix++];
    }
    else if (('0' <= c && c <= '9') ||
             (cp_char_ix == 0 && c == '-'))
    {
      cpn_str[cp_char_ix++] = c;
      c = fp_str[fp_str_ix++];
    }
    else
    {
      fprintf(stderr, "invalid character '%c' at position %d\n",
              c, fp_str_ix - 1);
      goto error;
    }
  }

  // based on cprint[cprint_ix++], cprint_ix == n items
  cprint_len = cprint_ix;

  fp = new_fprint(cprint_len);
  if (!fp)
    goto error;

  fp->cprint_len = cprint_len;
  fp->songlen = songlen;
  fp->bit_rate = bit_rate;
  fp->num_errors = num_errors;
  memcpy(fp->r, r, sizeof(r));
  memcpy(fp->dom, dom, sizeof(dom));
  memcpy(fp->cprint, cprint, cprint_len * sizeof(*cprint));

  if (cprint)
  {
    free(cprint);
    cprint = NULL;
  }

  return fp;

error:
  if (cprint)
  {
    free(cprint);
    cprint = NULL;
  }
  if (fp)
    free_fprint(fp);
  return NULL;
}
