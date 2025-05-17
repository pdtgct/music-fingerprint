/*
 *  fplib.h
 *
 *  interface to run a fingerprint using ffmpeg and libfooid
 *
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
 */

#ifndef _FPLIB_H
#define _FPLIB_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <libfooid/fooid.h>

// re-defined here because fooid.h does not export it
#define R_SIZE 348
#define R_SIZE8 R_SIZE * sizeof(uint8_t)
#define R_SIZE32 R_SIZE8 / sizeof(uint32_t)
#define DOM_SIZE 66
#define DOM_SIZE8 DOM_SIZE * sizeof(uint8_t)
#define DOM_LEN32 DOM_SIZE8 / sizeof(uint32_t)
#define DOM_END16 DOM_SIZE8 / sizeof(uint16_t) - 1

// based on 60-second samples
#define KNOWN_CPRINT_LEN 948

#if defined(__x86_64__) || defined(__ppc64__)
#define _64_BIT
#endif

  typedef struct FPrint
  {
    size_t cprint_len;
    uint32_t songlen;
    int32_t bit_rate;
    int32_t num_errors;
    uint8_t r[R_SIZE];
    uint8_t dom[DOM_SIZE];
    int32_t cprint[1];
  } FPrint;

  typedef struct FPrintUnion
  {
    size_t cprint_len;
    uint32_t min_songlen;
    int32_t bit_rate;
    uint32_t max_songlen;
    uint8_t r[R_SIZE];
    uint8_t dom[DOM_SIZE];
    int32_t cprint[1];
  } FPrintUnion;

#ifdef _64_BIT
  extern inline size_t max_st(size_t x, size_t y);
  extern inline size_t max_st(size_t x, size_t y)
  {
    return ((((size_t)(-((int64_t)(y < x)))) & (x ^ y)) ^ y);
  }
  extern inline size_t min_st(size_t x, size_t y);
  extern inline size_t min_st(size_t x, size_t y)
  {
    return ((((size_t)(-((int64_t)(y > x)))) & (x ^ y)) ^ y);
  }
#else
extern inline size_t max_st(size_t x, size_t y);
extern inline size_t max_st(size_t x, size_t y)
{
  return ((((size_t)(-((int32_t)(y < x)))) & (x ^ y)) ^ y);
}
extern inline size_t min_st(size_t x, size_t y);
extern inline size_t min_st(size_t x, size_t y)
{
  return ((((size_t)(-((int32_t)(y > x)))) & (x ^ y)) ^ y);
}
#endif
  extern inline uint32_t max_u32(uint32_t x, uint32_t y);
  extern inline uint32_t max_u32(uint32_t x, uint32_t y)
  {
    return ((((uint32_t)(-((int32_t)(y < x)))) & (x ^ y)) ^ y);
  }
  extern inline uint32_t min_u32(uint32_t x, uint32_t y);
  extern inline uint32_t min_u32(uint32_t x, uint32_t y)
  {
    return ((((uint32_t)(-((int32_t)(y > x)))) & (x ^ y)) ^ y);
  }

#define ABS(x) __builtin_abs((x))

#define CALC_FP_SIZE(cprint_len) \
  sizeof(FPrint) + (max_st((cprint_len), 1) - 1) * sizeof(int32_t)

#define FP_EXACT_CUTOFF 0.98
#define FP_ISNEQ(val) ((val) <= FP_EXACT_CUTOFF)
#define FP_ISEQ(val) ((val) > FP_EXACT_CUTOFF)
#define FP_MATCH_CUTOFF 0.6
#define FP_NOMATCH(val) ((val) <= FP_MATCH_CUTOFF)
#define FP_ISMATCH(val) ((val) > FP_MATCH_CUTOFF)

  FPrint *new_fprint(int cprint_len);

  void free_fprint(FPrint *fp);

  /*! get_fingerprint
   *  \brief return a t_fooid* FooID structure containing the fingerprint, or NULL
   *    \param   filename    const char* to an existing audio music file
   *    \param   error       int* will be set with error code on error
   *    \param   verbose     int, if nonzero print metadata to stdout
   *                         (temporary; may remove in future versions)
   */
  FPrint *get_fingerprint(const char *filename, int *error, int verbose);

  /*! ffmpeg_init
   *
   *  \brief Initialize ffmpeg structures; must be called once before
   *  calling ffmpeg functions (or get_fingerprint).  ffmpeg structures
   *  do not need to be de-initialized.
   */
  void ffmpeg_init(void);

  /*! hdist_r
   *
   *  \brief return the Hamming Distance for two t_fingerprint.r arrays
   */
  uint32_t hdist_r(const uint8_t *restrict r_a, const uint8_t *restrict r_b);

  uint32_t nhdist_r(const uint8_t *restrict r_a, const uint8_t *restrict r_b);

  /*! hdist_dom
   *
   *  \brief return the Hamming Distance for two t_fingerprint.dom arrays
   */
  uint32_t hdist_dom(const uint8_t *restrict dom_a,
                     const uint8_t *restrict dom_b);

  /*! match_fooid_fp
   */
  double match_fooid_fp(const uint8_t *restrict r_a,
                        const uint8_t *restrict dom_a,
                        const uint8_t *restrict r_b,
                        const uint8_t *restrict dom_b);

  /*!  match_chroma
   *   original reference implementation from Chromaprint
   */
  double match_chroma(const int32_t *restrict cp1, size_t cp1_len,
                      const int32_t *restrict cp2, size_t cp2_len,
                      size_t start, size_t end,
                      int *error);

  /*!  match_chromab
   *   return the absolute value of the sample correlation coefficient
   */
  double match_chromab(const int32_t *restrict cp1, size_t cp1_len,
                       const int32_t *restrict cp2, size_t cp2_len);

  // main implementation we use
  // match bit position of first bit set
  double match_chromac(const int32_t *restrict cp1, size_t cp1_len,
                       const int32_t *restrict cp2, size_t cp2_len);

  // Tanimoto
  double match_chromat(const int32_t *restrict cp1, size_t cp1_len,
                       const int32_t *restrict cp2, size_t cp2_len);

  double match_cpfm(FPrint *restrict a, FPrint *restrict b);

  void fprint_merge(FPrintUnion *restrict u,
                    const FPrint *restrict a,
                    const FPrint *restrict b);

  void fprint_merge_one(FPrintUnion *restrict u, const FPrint *restrict a);

  void fprint_merge_one_union(FPrintUnion *restrict u, const FPrintUnion *restrict a);

  float match_fprint_merge(const FPrint *restrict a, const FPrintUnion *restrict u);

  float match_merges(const FPrintUnion *restrict u1, const FPrintUnion *restrict u2);

  float try_match_merges(const FPrintUnion *restrict u1,
                         const FPrintUnion *restrict u2,
                         const FPrint *restrict a);

  char *fprint_to_string(const FPrint *fp);

  FPrint *fprint_from_string(const char *fp_str);

#ifdef __cplusplus
}
#endif

#endif /* _FPLIB_H */
