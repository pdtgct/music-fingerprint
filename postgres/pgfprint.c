/*
 *  pgfprint.c
 *  support library for PostgreSQL
 *
 *  Created by Peter Tanski on 27 June 2010.
 *  Copyright 2010 Zatisfi, LLC. MIT License, 2025
 *
 *  NOTE: before reviewing this code, see the GiST README in the PostgreSQL
 *        source at ${topdir}/src/backend/access/gist/README
 */

// always include postgres.h first, before even system headers
#include "postgres.h"
#include "fmgr.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libpq/pqformat.h"
#include "access/gist.h"
#include "access/skey.h"

#include "fplib.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

Datum fprint_in(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_in);
Datum fprint_out(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_out);
Datum fprint_compress(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_compress);
Datum fprint_decompress(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_decompress);
Datum fprint_union(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_union);
Datum fprint_picksplit(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_picksplit);
Datum fprint_consistent(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_consistent);
Datum fprint_penalty(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_penalty);
Datum fprint_same(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_same);

Datum fprint_cmp(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_cmp);
Datum fprint_eq(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_eq);
Datum fprint_neq(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_neq);
Datum fprint_match(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(fprint_match);

typedef struct
{
  char vl_len_[4];
  uint8_t data[1];
} fprint_gist;

#define MAX_KEY_CP_LEN 240
// #define KEY_CP_END_IX 480
//  NEW: 704 - 944 (secs 44-59)
#define KEY_CP_START_IX1 464
#define KEY_CP_END_IX1 704
#define KEY_CP_START_IX2 704
#define KEY_CP_END_IX2 944

#define DatumGetFPrint(p) ((FPrint *)(p))
#define SERIALIZED_FP(gp) DatumGetFPrint(VARDATA(gp))
#define CALC_GFP_SIZE(cprint_len) MAXALIGN(CALC_FP_SIZE(cprint_len) + VARHDRSZ)
#define SET_VARSIZE_GFP(gp, cprint_len) \
  SET_VARSIZE(gp, CALC_GFP_SIZE(cprint_len))
#define GET_GFP_ARG(argn) \
  (fprint_gist *)PG_DETOAST_DATUM(PG_GETARG_POINTER((argn)))

// Strategies -- see corresponding numbers in operator class of pgfprint.sql
#define FPStrategyEQ 3
#define FPStrategyNEQ 12
#define FPStrategySame 6

#ifdef DEBUG
#define FPDEBUG(msg)                                                             \
  do                                                                             \
  {                                                                              \
    ereport(NOTICE,                                                              \
            (errmsg_internal("[%s:%s:%d] " msg, __FILE__, __func__, __LINE__))); \
  } while (0);
#define FPDEBUG_M(msg, ...)                                                                   \
  do                                                                                          \
  {                                                                                           \
    ereport(NOTICE,                                                                           \
            (errmsg_internal("[%s:%s:%d] " msg, __FILE__, __func__, __LINE__, __VA_ARGS__))); \
  } while (0);
#else
#define FPDEBUG(msg)
#define FPDEBUG_M(msg, ...)
#endif

#if defined(_64_BIT) || defined(__APPLE__)
#define SIZE_T_FMT "%lu"
#else
#define SIZE_T_FMT "%u"
#endif

static inline void *checked_malloc(size_t size)
{
  void *ptr = NULL;
  ptr = malloc(size);
  if (ptr == NULL)
  {
    // malloc and related functions only set errno == ENOMEM on BSD (OS X)
    elog(ERROR, "[%s:%s:%d] unable to malloc size " SIZE_T_FMT,
         __FILE__, __func__, __LINE__, size);
    return NULL;
  }
  return ptr;
}

static inline FPrintUnion *check_union_size(FPrintUnion *fp_u, FPrint *fp_n)
{
  volatile size_t n_cplen = min_st(fp_n->cprint_len, MAX_KEY_CP_LEN);
  FPrintUnion *tmp = NULL;
  if (n_cplen > fp_u->cprint_len)
  {
    FPDEBUG_M("reallocating union to size %lu", CALC_FP_SIZE(n_cplen));
    tmp = realloc(fp_u, CALC_FP_SIZE(n_cplen));
    if (!tmp)
    {
      if (fp_u)
        free(fp_u);
      elog(ERROR, "[%s:%s:%d] unable to reallocate to new size " SIZE_T_FMT,
           __FILE__, __func__, __LINE__, CALC_FP_SIZE(n_cplen));
      return NULL;
    }
    fp_u = tmp;
    fp_u->cprint_len = n_cplen;
  }
  return fp_u;
}

/* deserialize_fprint
 * We decompress (detoast) anything that would have gone through decompress so:
 *  1. we avoid having GiST hold on to memory from another copy of an item
 *     that has been passed out of fprint_decompress
 *  2. to avoid any values that have been toasted in between decompress and
 *     the next function, or key values that were not passed through
 *     decompress but that were toasted
 *  3. special validation to catch memory errors.
 */
static inline FPrint *deserialize_fprint(Datum toasted)
{
  fprint_gist *gfp = (fprint_gist *)PG_DETOAST_DATUM(toasted);
  FPrint *fp = NULL;
  FPrint *nfp = NULL;
  size_t key_cp_len = MAX_KEY_CP_LEN;
  int start = 0;

  if ((gfp == NULL || VARSIZE(gfp) == 0))
  {
    return NULL;
  }

  fp = SERIALIZED_FP(gfp);

  if (fp && (fp->cprint_len < 100000))
  {
    key_cp_len = min_st(key_cp_len, fp->cprint_len);
    if (fp->cprint_len >= KEY_CP_END_IX2)
    {
      // secs 44-99
      start = KEY_CP_START_IX2;
    }
    else if (fp->cprint_len >= KEY_CP_END_IX1)
    {
      // secs 29.36-44.55
      start = KEY_CP_START_IX1;
    }
    nfp = checked_malloc(CALC_FP_SIZE(key_cp_len));
    if (nfp != NULL)
    {
      memcpy(nfp, fp, sizeof(*fp) - sizeof(fp->cprint[0]));
      memcpy(nfp->cprint, &fp->cprint[start],
             key_cp_len * sizeof(fp->cprint[0]));
      nfp->cprint_len = key_cp_len;
    }
  }
  else if (fp->cprint_len > 100000)
  {
    elog(ERROR, "[%s:%s:%d] detoasted fprint is invalid: cprint_len: " SIZE_T_FMT, __FILE__, __func__, __LINE__, fp->cprint_len);
  }

  // DO NOT PG_FREE_IF_COPY

  return nfp;
}

static inline FPrint *deserialize_fprint_full(Datum toasted)
{
  fprint_gist *gfp = (fprint_gist *)PG_DETOAST_DATUM(toasted);
  FPrint *fp = NULL;
  FPrint *nfp = NULL;

  if ((gfp == NULL || VARSIZE(gfp) == 0))
  {
    return NULL;
  }

  fp = SERIALIZED_FP(gfp);

  if (fp && (fp->cprint_len < 100000))
  {
    // (VARSIZE(gfp)-VARHDRSZ) >= CALC_FP_SIZE(fp->cprint_len)
    nfp = checked_malloc(VARSIZE(gfp));
    if (nfp != NULL)
    {
      memcpy(nfp, fp, CALC_FP_SIZE(fp->cprint_len));
    }
  }
  else if (fp->cprint_len > 100000)
  {
    elog(ERROR, "[%s:%s:%d] detoasted fprint is invalid: cprint_len: " SIZE_T_FMT, __FILE__, __func__, __LINE__, fp->cprint_len);
  }

  // DO NOT PG_FREE_IF_COPY

  return nfp;
}

#define BASEFMT "(%u,%u,%u,"

Datum fprint_in(PG_FUNCTION_ARGS)
{
  char *fp_str = PG_GETARG_CSTRING(0);
  fprint_gist *gfp = NULL;
  FPrint *fp = NULL;
  uint32_t songlen = 0;
  uint32_t bit_rate = 0;
  uint32_t num_errors = 0;
  uint8_t r[R_SIZE];
  uint8_t dom[DOM_SIZE];
  int32_t *cprint = NULL;
  int32_t *tmp = NULL;
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
    PG_RETURN_NULL();

  //   7 for minimum: "(0,0,0,"
  // + 2 ",," after R and DOM
  // + 2 "0)" for minimum cprint and finishing ')'
  if ((fp_str_len = strlen(fp_str)) < (11 + 2 * R_SIZE + 2 * DOM_SIZE))
  {
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("invalid string length: %d\n", fp_str_len)));
  }

  nret = sscanf(fp_str, BASEFMT, &songlen, &bit_rate, &num_errors);
  if (nret != 3)
  {
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("string must begin with 3 arguments\n")));
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
      ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                      errmsg("invalid format for r block at character %d\n",
                             fp_str_ix)));
    }
    fp_str_ix += 8;
  }
  if (fp_str[fp_str_ix++] != ',')
  {
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("missing ',' after r block\n")));
  }

  for (int i = 0; i < DOM_SIZE; i += 2)
  {
    nret = sscanf(&fp_str[fp_str_ix], "%2hhX%2hhX", &dom[i], &dom[i + 1]);
    if (nret != 2)
    {
      ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                      errmsg("invalid format for dom block at character %d\n",
                             fp_str_ix)));
    }
    fp_str_ix += 4;
  }
  if (fp_str[fp_str_ix++] != ',')
  {
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("missing ',' after dom block\n")));
  }

  cprint = palloc(KNOWN_CPRINT_LEN * sizeof(*cprint));
  cprint_len = KNOWN_CPRINT_LEN;

  c = fp_str[fp_str_ix++];
  while (c != '\0')
  {
    if (cp_char_ix >= 12)
    {
      ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                      errmsg("integer ending at position %d is too wide\n",
                             fp_str_ix - 1)));
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
        tmp = repalloc(cprint, cprint_len * sizeof(*cprint));
        if (!tmp)
        {
          if (cprint)
            pfree(cprint);
          elog(ERROR,
               "[%s:%s:%d] unable to reallocate cprint to new size " SIZE_T_FMT, __FILE__, __func__, __LINE__,
               cprint_len * sizeof(*cprint));
          PG_RETURN_NULL();
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
      ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                      errmsg("invalid character '%c' at position %d\n",
                             c, fp_str_ix - 1)));
    }
  }

  // based on cprint[cprint_ix++], cprint_ix == n items
  cprint_len = cprint_ix;

  gfp = palloc(CALC_GFP_SIZE(cprint_len));
  SET_VARSIZE_GFP(gfp, cprint_len);
  fp = SERIALIZED_FP(gfp);
  fp->cprint_len = cprint_len;
  fp->songlen = songlen;
  fp->bit_rate = bit_rate;
  fp->num_errors = num_errors;
  memcpy(fp->r, r, sizeof(r));
  memcpy(fp->dom, dom, sizeof(dom));
  memcpy(fp->cprint, cprint, cprint_len * sizeof(*cprint));

  if (cprint)
  {
    pfree(cprint);
    cprint = NULL;
  }

  PG_RETURN_POINTER(gfp);
}

// max 24 (10 each for songlen, bit_rate, num_errors) + 3 for "(,," + 1 '\0'
#define BASE_SIZE 24

Datum fprint_out(PG_FUNCTION_ARGS)
{
  fprint_gist *gfp = GET_GFP_ARG(0);
  FPrint *fp = NULL;
  char *tmpstr = NULL;
  int out_len = 0;
  char base[BASE_SIZE];
  size_t cprint_len;
  uint8_t *r = NULL;
  uint8_t *dom = NULL;
  int32_t *cprint = NULL;
  int base_sz = 0;
  size_t str_sz = 0;
  char *outstr = NULL;

  if (!gfp)
    PG_RETURN_NULL();

  fp = SERIALIZED_FP(gfp);

  cprint_len = fp->cprint_len;
  r = fp->r;
  dom = fp->dom;
  cprint = fp->cprint;
  base_sz = snprintf(base, BASE_SIZE, BASEFMT,
                     fp->songlen, fp->bit_rate, fp->num_errors);
  // + 2 for following ")\0"; 9 to allow for '-'
  str_sz = base_sz + (2 * R_SIZE + 1) + (2 * DOM_SIZE + 1) + (12 * cprint_len) + 2;

  tmpstr = palloc(str_sz * sizeof(char));

  (void)strncpy(tmpstr, base, str_sz);
  // chop terminating "\0"
  out_len += base_sz;

  for (int i = 0; i < R_SIZE; i++)
  {
    out_len += snprintf(&tmpstr[out_len], 3, "%02X", r[i]);
  }
  (void)strncpy(&tmpstr[out_len++], ",", 2);

  for (int i = 0; i < DOM_SIZE; i++)
  {
    out_len += snprintf(&tmpstr[out_len], 3, "%02X", dom[i]);
  }
  (void)strncpy(&tmpstr[out_len++], ",", 2);

  for (size_t j = 0; j < cprint_len; j++)
  {
    // 32-bit int max size is 10 chars + 2 ("-"? and " \0")
    out_len += snprintf(&tmpstr[out_len], 13, "%d ", cprint[j]);
  }
  // cheat: pull back from final " "
  out_len -= 1;
  strncpy(&tmpstr[out_len++], ")", 2);

  // only the input pointer here is checked; otherwise this is a
  // bare call to realloc()
  outstr = repalloc(tmpstr, ((size_t)out_len + 1) * sizeof(*tmpstr));
  if (!outstr)
  {
    if (tmpstr)
    {
      pfree(tmpstr);
    }
    elog(ERROR, "[%s:%s:%d] unable to reallocate to new size " SIZE_T_FMT,
         __FILE__, __func__, __LINE__,
         ((size_t)out_len + 1) * sizeof(*tmpstr));
  }

  PG_FREE_IF_COPY(gfp, 0);

  PG_RETURN_CSTRING(outstr);
}

Datum fprint_compress(PG_FUNCTION_ARGS)
{
  GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
  GISTENTRY *retval = entry;
  fprint_gist *gfp_in = NULL;
  FPrint *fp_in = NULL;
  fprint_gist *gfp_out = NULL;
  FPrint *fp_out = NULL;
  size_t key_cp_len = MAX_KEY_CP_LEN;
  int start = 0;

  // entry->leafkey == TRUE if coming from table
  if (!entry->leafkey)
  {
    PG_RETURN_POINTER(retval);
  }

  // recommendation is to palloc a new entry, even if gistcentryinit
  // does not use it
  retval = palloc(sizeof(*retval));

  if (!entry->key)
  {
    elog(ERROR, "compress got NULL DatumGetPointer(entry->key)");
    // should break out of function here and not return, but just in case..
    PG_RETURN_POINTER(retval);
  }

  gfp_in = (fprint_gist *)PG_DETOAST_DATUM(entry->key);
  if (!gfp_in)
  {
    elog(ERROR, "PG_DETOAST_DATUM(<notnull>) returned NULL");
    PG_RETURN_POINTER(entry);
  }
  fp_in = SERIALIZED_FP(gfp_in);

  key_cp_len = min_st(key_cp_len, fp_in->cprint_len);
  if (fp_in->cprint_len >= KEY_CP_END_IX2)
  {
    // secs 44-99
    start = KEY_CP_START_IX2;
  }
  else if (fp_in->cprint_len >= KEY_CP_END_IX1)
  {
    // secs 30-45
    start = KEY_CP_START_IX1;
  }
  gfp_out = palloc(CALC_GFP_SIZE(key_cp_len));
  SET_VARSIZE_GFP(gfp_out, key_cp_len);
  fp_out = (FPrint *)VARDATA(gfp_out);
  memcpy(fp_out, fp_in, sizeof(*fp_in) - sizeof(fp_in->cprint[0]));
  memcpy(fp_out->cprint, &fp_in->cprint[start],
         key_cp_len * sizeof(fp_in->cprint[0]));
  fp_out->cprint_len = key_cp_len;

  gistentryinit(*retval, PointerGetDatum(gfp_out),
                entry->rel, entry->page, entry->offset, FALSE);

  // PG_FREE_IF_COPY
  if ((Pointer)gfp_in != (Pointer)(entry->key))
    pfree(gfp_in);

  PG_RETURN_POINTER(retval);
}

Datum fprint_decompress(PG_FUNCTION_ARGS)
{
  GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);

  if (!entry)
  {
    elog(ERROR, "fprint_decompress: entry is NULL");
  }

  // cut out here -- we handle the memory
  PG_RETURN_POINTER(entry);
}

/* fprint_union
 * Called to build or merge key values for Nodes of the tree.
 * The output from this function will be passed directly to fprint_same
 * to compare keys.
 *
 * Since we need some criterion for determining whether two Node keys are
 * different, we try to include the song length in the comparison.
 * This gives the resulting index a kind of linear order between different
 * comparison fields, though really an R-Tree is sub-optimal in this case
 * where we really need a graph.
 *
 * DECOMPRESS: all entries in entvec.
 *
 */
Datum fprint_union(PG_FUNCTION_ARGS)
{
  GistEntryVector *entryvec = (GistEntryVector *)PG_GETARG_POINTER(0);
  int *size = (int *)PG_GETARG_POINTER(1);
  GISTENTRY *entv = entryvec->vector;
  fprint_gist *gret = NULL;
  FPrintUnion *ret = NULL;
  FPrintUnion *v = NULL;
  OffsetNumber n_entries = entryvec->n;
  OffsetNumber i = 0;

  if (n_entries > 2)
  {
    FPDEBUG_M("entryvec->n: %d", n_entries);
  }

  v = (FPrintUnion *)deserialize_fprint(entv[i].key);
  if (v == NULL)
  {
    elog(ERROR, "[%s:%s:%d] first entry to union is invalid",
         __FILE__, __func__, __LINE__);
    PG_RETURN_NULL();
  }

  ret = calloc(CALC_FP_SIZE(v->cprint_len), 1);
  if (ret == NULL)
  {
    elog(ERROR, "[%s:%s:%d] unable to malloc size " SIZE_T_FMT,
         __FILE__, __func__, __LINE__, CALC_FP_SIZE(v->cprint_len));
    PG_RETURN_NULL();
  }
  memcpy(ret, v, CALC_FP_SIZE(v->cprint_len));
  if (v)
    free(v);

  // should not matter whether entry is a leaf or key here, since
  // key | leaf == key->leaves[i] | key->leaves[i+1] .. | leaf
  for (i = 1; i < n_entries; i++)
  {
    v = (FPrintUnion *)deserialize_fprint(entv[i].key);
    if (v == NULL)
    {
      elog(ERROR, "unable to deserialize union");
      continue;
    }
    ret = check_union_size(ret, (FPrint *)v);
    if (!ret)
    {
      if (v)
        free(v);
      PG_RETURN_NULL();
    }

    fprint_merge_one_union(ret, v);

    if (v)
    {
      free(v);
      v = NULL;
    }
  }

  gret = palloc(CALC_GFP_SIZE(ret->cprint_len));
  SET_VARSIZE_GFP(gret, ret->cprint_len);
  memcpy(VARDATA(gret), ret, CALC_FP_SIZE(ret->cprint_len));
  *size = VARSIZE(gret);

  if (ret)
    free(ret);

  PG_RETURN_POINTER(gret);
}

/*  PickSplit Notes
 *  ---------------
 *
 *  This function is called when an old index page is full and the overflow
 *  should be sent to a new index page.  PickSplit is run when an index page
 *  size is >= the FILLFACTOR.  See,
 *  http://www.postgresql.org/docs/8.4/interactive/sql-createtable.html#SQL-CREATETABLE-STORAGE-PARAMETERS.
 *  The FILLFACTOR is a percentage of the space filled (100 == 100% packing).
 *  GiST does not, however, support fillfactor so we are stuck with the preset
 *  threshold, below.
 *
 *  The page size in Postgres, hard-coded at installation time (from initdb),
 *  defaults to 8 kB.  The typical size of a FPrint struct is:
 *    FPrint:
 *                  32-bit | 64-bit
 *      cprint_len       4 |      8
 *      songlen          4
 *      bit_rate         4
 *      num_errors       4
 *      r[348]         348
 *      dom[66]         66
 *      cprint[]      3792
 *                   -----  | -----
 *                   4222   | 4230
 *
 *  The actual datum size must be less than 4240 bytes to fit on a GiST
 *  page so our typical data size includes 240 int32_t (cprint[240]) for a
 *  total of 1630 or 1634 bytes.  FPrintUnion structs have the same
 *  binary representation so the sizes are the same for key values.
 *
 *  If we used the raw data type we would only be able to hold one or at
 *  most two entries on each index page.  Instead, we set the data type to
 *  be TOASTable (the data type is EXTENDED).  The total size of a TOAST
 *  pointer datum is guaranteed to be 18 bytes but generally the number of
 *  entries per page is 4.
 *
 *  In the end, the size of each half of the split (left and right) must be
 *  greater than 1.  If the left or right half of the GIST_SPLITVECTOR *v
 *  contains only 1 entry the gistfindleaf() (see gist.c:533) may run into
 *  an infinite loop as it attempts to choose an optimal entry point for a
 *  new row among a set of pages of size 1.
 *
 *  Since the typical page size is 4 and overflow occurs at 5, and since
 *  a basic requirement of a GiST page is >= 2 entries (1 entry will lead
 *  to an infinite loop), the best we can do is a 50/50 split.  The most
 *  frequent split is 3/3: 3 - left, 3 - right, though for collections of
 *  short songs there may be as many as 20 - left, 20 - right. It is
 *  important for the algorithm to remain relatively efficient and flexible.
 *
 *  DECOMPRESS: all entries in entryvec
 *
 *  Notes on Splitting Algorithm
 *  ----------------------------
 *
 *  GiST requires an R-tree splitting algorithm.  That is, the split should
 *  maximize the distance between the two Nodes while clustering the leaves
 *  of each Node around a centroid as much as possible.  For GiST, it is
 *  more crucial to differentiate Nodes as that will affect the Penalty
 *  for following each branch of the tree.  If a split results in both sides
 *  having the same cost for inserting a value, GiST will go into an infinite
 *  loop.  For a general overview of R-Tree splitting algorithms, see:
 *  http://donar.umiacs.umd.edu/quadtree/docs/rtree_split_rules.html
 *
 *  The algorithm below is fairly simple.  It is an implementation of Guttman's
 *  poly-time split algorithm used in many of the GiST contributed modules:
 *   - match each fingerprint against all the others and store in an array of
 *     recording the matches made.
 *   - sort the matches in ascending order so the the two furthest apart
 *     (most different) are at the top, using the (FPrint*)fp->songlen as the
 *     primary comparison and the fingerprint matches as the tie-breaker..
 *   - the top Match (two entries most different) are the seeds for the left
 *     and right branches; those seeds are used to create the Union values
 *     that contain all the values in each branch.
 *   - using the new seed-Unions, match all items again as the difference
 *     each would make if added to either branch.
 *   - sort the new matches in ascending order to so the entries that would
 *     make the least difference if added to either side are first in the list.
 *   - start adding the entries (except the seeds) to either side
 *   - handle any leftover matches by splitting them evenly across nodes;
 *     again, in practice we generally never get that far.
 *
 */

// used in fprint_picksplit to coordinate matches
typedef struct Match
{
  int ix1;
  int ix2;
  uint32_t songlen_diff;
  double val;
} Match;

// Comparator function for qsort over an array of Match*.
// Duplicative declaration avoids an annoying compiler warning.
int cmp_matches(const void *match1, const void *match2);
int cmp_matches(const void *match1, const void *match2)
{
  Match *m1 = (Match *)match1;
  Match *m2 = (Match *)match2;
  if (m1->songlen_diff > m2->songlen_diff)
    return 1;
  if (m1->songlen_diff < m2->songlen_diff)
    return -1;
  if (m1->val > m2->val)
    return 1;
  if (m1->val < m2->val)
    return -1;
  return 0;
}

#define ASSIGN_IX(ix, fpx, fp_u, side, n_side) \
  do                                           \
  {                                            \
    (side)[(n_side)++] = (ix) + 1;             \
    (fp_u) = check_union_size((fp_u), (fpx));  \
    if (!(fp_u))                               \
      goto picksplit_cleanup;                  \
    fprint_merge_one((fp_u), (fpx));           \
  } while (0)

#define ASSIGN_IXU(ix, fpux, fp_u, side, n_side)         \
  do                                                     \
  {                                                      \
    (side)[(n_side)++] = (ix) + 1;                       \
    (fp_u) = check_union_size((fp_u), (FPrint *)(fpux)); \
    if (!(fp_u))                                         \
      goto picksplit_cleanup;                            \
    fprint_merge_one_union((fp_u), (fpux));              \
  } while (0)

// really doesn't add much; used in contrib/hstore/hstore_gist.c
#define WISH_F(a, b, c) (double)(-(double)(((a) - (b)) * ((a) - (b)) * ((a) - (b))) * (c))

Datum fprint_picksplit(PG_FUNCTION_ARGS)
{
  GistEntryVector *entryvec = (GistEntryVector *)PG_GETARG_POINTER(0);
  GIST_SPLITVEC *v = (GIST_SPLITVEC *)PG_GETARG_POINTER(1);
  GISTENTRY *entv = entryvec->vector;
  OffsetNumber maxoff = entryvec->n - 1;
  OffsetNumber *left = NULL;
  OffsetNumber *right = NULL;
  volatile OffsetNumber i;
  volatile int j, k;
  fprint_gist *gfp_ul = NULL;
  fprint_gist *gfp_ur = NULL;
  FPrintUnion *fp_ul = NULL;
  FPrintUnion *fp_ur = NULL;
  FPrint *fp1 = NULL;
  FPrint *fp2 = NULL;
  FPrintUnion *fpu1 = NULL;
  FPrintUnion *fpu2 = NULL;
  FPrint **raw_vec = NULL;
  Match *m = NULL;
  Match **matches = NULL;
  int n_left = 0;
  int n_right = 0;
  size_t n_entries = maxoff;
  int max_clust_sz = (int)(n_entries % 2 == 0 ? n_entries >> 1 : (n_entries >> 1) + 1);
  size_t n_bytes = (n_entries + 1) * sizeof(OffsetNumber);
  size_t n_matches = 0;
  bool leaf_split = true;
  bool allisequal = true;
  int seed_left, seed_right;
  double tmatch_left, tmatch_right;
  uint32_t min_songlen, max_songlen;

  left = v->spl_left = (OffsetNumber *)palloc(n_bytes);
  v->spl_nleft = 0;

  right = v->spl_right = (OffsetNumber *)palloc(n_bytes);
  v->spl_nright = 0;

  raw_vec = checked_malloc(n_entries * sizeof(*raw_vec));
  if (!raw_vec)
    goto picksplit_cleanup;

  j = 0;
  i = FirstOffsetNumber;
  // it seems this must be done before testing GIST_LEAF
  fp1 = deserialize_fprint(entv[i].key);
  if (fp1 == NULL)
  {
    elog(ERROR, "entry %d is invalid", i);
    goto picksplit_cleanup;
  }
  if (!GIST_LEAF(&entv[i]))
  {
    leaf_split = false;
  }
  raw_vec[j++] = fp1;
  seed_left = seed_right = 0;

  if (leaf_split)
  {
    min_songlen = max_songlen = fp1->songlen;
    for (i = OffsetNumberNext(i); i <= maxoff; i = OffsetNumberNext(i))
    {
      fp1 = deserialize_fprint(entv[i].key);
      if (fp1 == NULL)
      {
        elog(ERROR, "entry %d is invalid", i);
        goto picksplit_cleanup;
      }
      j++;
      raw_vec[i - 1] = fp1;
      if (min_songlen > fp1->songlen)
      {
        seed_left = i - 1;
        min_songlen = fp1->songlen;
        allisequal = false;
      }
      else if (max_songlen < fp1->songlen)
      {
        seed_right = i - 1;
        max_songlen = fp1->songlen;
        allisequal = false;
      }
    }
  }
  else
  {
    fpu1 = (FPrintUnion *)fp1;
    min_songlen = fpu1->min_songlen;
    max_songlen = fpu1->max_songlen;
    for (i = OffsetNumberNext(i); i <= maxoff; i = OffsetNumberNext(i))
    {
      fpu1 = (FPrintUnion *)deserialize_fprint(entv[i].key);
      if (fpu1 == NULL)
      {
        elog(ERROR, "entry %d is invalid", i);
        goto picksplit_cleanup;
      }
      j++;
      raw_vec[i - 1] = (FPrint *)fpu1;
      if (min_songlen > fpu1->min_songlen)
      {
        seed_left = i - 1;
        min_songlen = fpu1->min_songlen;
        allisequal = false;
      }
      else if (max_songlen < fpu1->max_songlen)
      {
        seed_right = i - 1;
        max_songlen = fpu1->max_songlen;
        allisequal = false;
      }
    }
  }
  if (n_entries > j)
  {
    elog(WARNING, "[%s:%s:%d]: " SIZE_T_FMT " bad entries",
         __FILE__, __func__, __LINE__, n_entries - j);
    n_entries = j;
  }
  else if (n_entries < j)
  {
    elog(ERROR, "skipping " SIZE_T_FMT " entries", j - n_entries);
  }

  if (n_entries < 3)
  {
    // n_entries == 1 may occurr when Datum size close to 4240 bytes
    // (max GiST page size)
    if (n_entries == 1)
    {
      elog(ERROR, "number of entries passed to picksplit is 1");
    }

    if (allisequal)
    {
      fp1 = raw_vec[0];
      fp2 = raw_vec[1];
      left[0] = 1;
      right[0] = 2;
    }
    else
    {
      fp1 = raw_vec[seed_left];
      fp2 = raw_vec[seed_right];
      left[0] = seed_left + 1;
      right[0] = seed_right + 1;
    }

    gfp_ul = palloc(CALC_GFP_SIZE(fp1->cprint_len));
    SET_VARSIZE_GFP(gfp_ul, fp1->cprint_len);
    fp_ul = (FPrintUnion *)VARDATA(gfp_ul);
    memcpy(fp_ul, fp1, CALC_FP_SIZE(fp1->cprint_len));
    gfp_ur = palloc(CALC_GFP_SIZE(fp2->cprint_len));
    SET_VARSIZE_GFP(gfp_ur, fp2->cprint_len);
    fp_ur = (FPrintUnion *)VARDATA(gfp_ur);
    memcpy(VARDATA(gfp_ur), fp2, CALC_FP_SIZE(fp2->cprint_len));

    if (leaf_split)
    {
      fp_ul->min_songlen = fp_ul->max_songlen = fp1->songlen;
      fp_ur->min_songlen = fp_ur->max_songlen = fp2->songlen;
    }

    v->spl_ldatum = PointerGetDatum(gfp_ul);
    v->spl_nleft = 1;

    v->spl_rdatum = PointerGetDatum(gfp_ur);
    v->spl_nright = 1;

    FPDEBUG("n_entries == 2");

    // reset fp_ul, fp_ur to NULL here to avoid freeing invalid pointers, below
    fp_ul = fp_ur = NULL;

    goto picksplit_cleanup;
  }

  n_matches = (n_entries * (n_entries - 1)) / 2;
  matches = checked_malloc(n_matches * sizeof(void *));
  if (!matches)
    goto picksplit_cleanup;
  for (k = 0; k < n_matches; k++)
  {
    m = checked_malloc(sizeof(*m));
    if (!m)
      goto picksplit_cleanup;
    matches[k] = m;
  }

  j = 0;
  if (allisequal)
  {
    // all the same songlen --> match by comparison
    if (leaf_split)
    {

      for (k = 0; k < n_entries; k++)
      {
        fp1 = raw_vec[k];
        for (int l = k + 1; l < n_entries; l++)
        {
          fp2 = raw_vec[l];
          m = matches[j++];
          m->ix1 = k;
          m->ix2 = l;
          m->songlen_diff = 0;
          m->val = match_cpfm(fp1, fp2);
        }
      }
    }
    else
    {

      for (k = 0; k < n_entries; k++)
      {
        fpu1 = (FPrintUnion *)raw_vec[k];
        for (int l = k + 1; l < n_entries; l++)
        {
          fpu2 = (FPrintUnion *)raw_vec[l];
          m = matches[j++];
          m->ix1 = k;
          m->ix2 = l;
          m->songlen_diff = 0;
          m->val = match_fprint_merge((FPrint *)fpu1, fpu2);
        }
      }
    }
    qsort((void *)matches, n_matches, sizeof(void *), cmp_matches);

    if (matches[n_matches - 1]->val > 0.4)
      allisequal = false;

    if (allisequal)
    {

      fp1 = raw_vec[0];
      fp2 = raw_vec[n_entries - 1];

      fp_ul = calloc(CALC_FP_SIZE(fp1->cprint_len), 1);
      if (!fp_ul)
      {
        elog(ERROR, "fprint_picksplit: allocating " SIZE_T_FMT " bytes for fp_ul",
             CALC_FP_SIZE(fp1->cprint_len));
        goto picksplit_cleanup;
      }
      memcpy(fp_ul, fp1, CALC_FP_SIZE(fp1->cprint_len));
      fp_ul->max_songlen = fp_ul->min_songlen = min_songlen;

      fp_ur = calloc(CALC_FP_SIZE(fp2->cprint_len), 1);
      if (!fp_ur)
      {
        elog(ERROR, "fprint_picksplit: allocating " SIZE_T_FMT " bytes for fp_ul",
             CALC_FP_SIZE(fp2->cprint_len));
        goto picksplit_cleanup;
      }
      memcpy(fp_ur, fp2, CALC_FP_SIZE(fp2->cprint_len));
      fp_ur->min_songlen = fp_ur->max_songlen = max_songlen;

      left[0] = 1;
      right[0] = n_entries;
      n_left++;
      n_right++;

      if (leaf_split)
      {
        for (k = 1; k < n_entries - 1; k++)
        {
          fp1 = raw_vec[k];
          if (k < max_clust_sz)
            ASSIGN_IX(k, fp1, fp_ul, left, n_left);
          else
            ASSIGN_IX(k, fp1, fp_ur, right, n_right);
        }
      }
      else
      {
        for (k = 1; k < n_entries - 1; k++)
        {
          fpu1 = (FPrintUnion *)raw_vec[k];
          if (k < max_clust_sz)
            ASSIGN_IXU(k, fpu1, fp_ul, left, n_left);
          else
            ASSIGN_IXU(k, fpu1, fp_ur, right, n_right);
        }
      }
      goto picksplit_assign;
    }
    // fall through to regular test, below
    m = matches[0];
    seed_left = m->ix1;
    seed_right = m->ix2;
  }

  fp1 = raw_vec[seed_left];
  fp2 = raw_vec[seed_right];

  fp_ul = calloc(CALC_FP_SIZE(fp1->cprint_len), 1);
  if (!fp_ul)
  {
    elog(ERROR, "fprint_picksplit: allocating " SIZE_T_FMT " bytes for fp_ul",
         CALC_FP_SIZE(fp1->cprint_len));
    goto picksplit_cleanup;
  }
  memcpy(fp_ul, fp1, CALC_FP_SIZE(fp1->cprint_len));
  fp_ul->max_songlen = fp_ul->min_songlen = min_songlen;

  fp_ur = calloc(CALC_FP_SIZE(fp2->cprint_len), 1);
  if (!fp_ur)
  {
    elog(ERROR, "fprint_picksplit: allocating " SIZE_T_FMT " bytes for fp_ul",
         CALC_FP_SIZE(fp2->cprint_len));
    goto picksplit_cleanup;
  }
  memcpy(fp_ur, fp2, CALC_FP_SIZE(fp2->cprint_len));
  fp_ur->min_songlen = fp_ur->max_songlen = max_songlen;

  left[n_left++] = seed_left + 1;
  right[n_right++] = seed_right + 1;

  j = 0;
  if (leaf_split)
  {
    for (k = 0; k < n_entries; k++)
    {
      fp1 = raw_vec[k];
      m = matches[k];
      m->ix1 = k;
      // if input were the greatest difference between the two,
      // sort ascending would put the ones in the middle first in the list
      m->songlen_diff = min_u32(fp1->songlen - min_songlen,
                                max_songlen - fp1->songlen);
      // here, the ones with the furthest match from the other side are last
      // in the list
      tmatch_left = try_match_merges(fp_ur, fp_ul, fp1);
      tmatch_right = try_match_merges(fp_ul, fp_ur, fp1);
      m->val = fmin((double)tmatch_left, (double)tmatch_right);
    }
    qsort((void *)matches, n_entries, sizeof(void *), cmp_matches);

    for (int l = 0; l < n_entries; l++)
    {
      m = matches[l];
      k = m->ix1;
      if (k == seed_left || k == seed_right)
        continue;
      fp1 = raw_vec[k];
      if (fp1->songlen - min_songlen < max_songlen - fp1->songlen)
      {
        ASSIGN_IX(k, fp1, fp_ul, left, n_left);
      }
      else if (fp1->songlen - min_songlen > max_songlen - fp1->songlen)
      {
        ASSIGN_IX(k, fp1, fp_ur, right, n_right);
      }
      else
      {
        tmatch_left = try_match_merges(fp_ur, fp_ul, fp1);
        tmatch_right = try_match_merges(fp_ul, fp_ur, fp1);
        if (tmatch_left < tmatch_right + WISH_F(n_left, n_right, 0.1))
        {
          ASSIGN_IX(k, fp1, fp_ul, left, n_left);
        }
        else if (tmatch_left > tmatch_right)
        {
          ASSIGN_IX(k, fp1, fp_ur, right, n_right);
        }
        else if (n_left < n_right)
        {
          ASSIGN_IX(k, fp1, fp_ul, left, n_left);
        }
        else
        {
          ASSIGN_IX(k, fp1, fp_ur, right, n_right);
        }
      }
    }
  }
  else
  {
    for (k = 0; k < n_entries; k++)
    {
      fpu1 = (FPrintUnion *)raw_vec[k];
      m = matches[j++];
      m->ix1 = k;
      // diff would be maximum expansion, left or right:
      m->songlen_diff = min_u32(fpu1->max_songlen - min_songlen,
                                max_songlen - fpu1->min_songlen);
      // try_match_merges does not reference songlen
      tmatch_left = try_match_merges(fp_ur, fp_ul, (FPrint *)fpu1);
      tmatch_right = try_match_merges(fp_ul, fp_ur, (FPrint *)fpu1);
      m->val = fmin((double)tmatch_left, (double)tmatch_right);
    }
    qsort((void *)matches, n_entries, sizeof(void *), cmp_matches);

    for (int l = 0; l < n_entries; l++)
    {
      m = matches[l];
      k = m->ix1;
      if (k == seed_left || k == seed_right)
        continue;
      fpu1 = (FPrintUnion *)raw_vec[k];
      if (fpu1->max_songlen - min_songlen < max_songlen - fpu1->min_songlen)
      {
        ASSIGN_IXU(k, fpu1, fp_ul, left, n_left);
      }
      else if (fpu1->max_songlen - min_songlen > max_songlen - fpu1->min_songlen)
      {
        ASSIGN_IXU(k, fpu1, fp_ur, right, n_right);
      }
      else
      {
        tmatch_left = try_match_merges(fp_ur, fp_ul, (FPrint *)fpu1);
        tmatch_right = try_match_merges(fp_ul, fp_ur, (FPrint *)fpu1);
        if (tmatch_left < tmatch_right + WISH_F(n_left, n_right, 0.1))
        {
          ASSIGN_IXU(k, fpu1, fp_ul, left, n_left);
        }
        else if (tmatch_left > tmatch_right)
        {
          ASSIGN_IXU(k, fpu1, fp_ur, right, n_right);
        }
        else if (n_left < n_right)
        {
          ASSIGN_IXU(k, fpu1, fp_ul, left, n_left);
        }
        else
        {
          ASSIGN_IXU(k, fpu1, fp_ur, right, n_right);
        }
      }
    }
  }

picksplit_assign:

  gfp_ul = palloc(CALC_GFP_SIZE(fp_ul->cprint_len));
  SET_VARSIZE_GFP(gfp_ul, fp_ul->cprint_len);
  memcpy(VARDATA(gfp_ul), fp_ul, CALC_FP_SIZE(fp_ul->cprint_len));
  v->spl_ldatum = PointerGetDatum(gfp_ul);
  v->spl_nleft = n_left;

  gfp_ur = palloc(CALC_GFP_SIZE(fp_ur->cprint_len));
  SET_VARSIZE_GFP(gfp_ur, fp_ur->cprint_len);
  memcpy(VARDATA(gfp_ur), fp_ur, CALC_FP_SIZE(fp_ur->cprint_len));
  v->spl_rdatum = PointerGetDatum(gfp_ur);
  v->spl_nright = n_right;

  FPDEBUG_M("leaf_split: %s split: left %d [%u,%u], right %d [%u,%u] -- %s",
            leaf_split ? "true" : "false",
            n_left, fp_ul->min_songlen, fp_ul->max_songlen,
            n_right, fp_ur->min_songlen, fp_ur->max_songlen,
            fp_ul->max_songlen > fp_ur->min_songlen ? "invalid" : "valid");

picksplit_cleanup:

  if (fp_ul)
  {
    free(fp_ul);
    fp_ul = NULL;
  }
  if (fp_ur)
  {
    free(fp_ur);
    fp_ur = NULL;
  }

  if (matches)
  {
    for (k = 0; k < n_matches; k++)
    {
      if (matches[k])
      {
        free(matches[k]);
        matches[k] = NULL;
      }
    }
    free(matches);
    matches = NULL;
  }
  if (raw_vec)
  {
    for (k = 0; k < n_entries; k++)
    {
      if (raw_vec[k])
      {
        free(raw_vec[k]);
        raw_vec[k] = NULL;
      }
    }
    free(raw_vec);
    raw_vec = NULL;
  }

  PG_RETURN_POINTER(v);
}

/* Penalty
 * -------
 * Adds a penalty to orig_key when combined with new_key.  In the case where
 * new_key is NULL, the expected penalty is 0.
 *
 * Supposedly, this is the only place we should have to handle NULLs.
 * It would be nice if we could return some flag and have GiST clean NULLs.
 *
 * According to the documentation, if this function is marked STRICT, it
 * should not have to deal with NULL but we still check.
 *
 * DECOMPRESS:
 *  new_fp   -- item `it` in gistutil.c:gistchoose(); a real FPrint
 *  orig_fp  -- Node union-Fprint key to compare against
 */
Datum fprint_penalty(PG_FUNCTION_ARGS)
{
  GISTENTRY *orig_ge = (GISTENTRY *)PG_GETARG_POINTER(0);
  GISTENTRY *new_ge = (GISTENTRY *)PG_GETARG_POINTER(1);
  FPrintUnion *orig_fp = (FPrintUnion *)deserialize_fprint(orig_ge->key);
  FPrint *new_fp = deserialize_fprint(new_ge->key);
  float *penalty = (float *)PG_GETARG_POINTER(2);
  float match = 0.0f;
  float songlen_diff = 0.0f;
  uint32_t new_songlen = new_fp->songlen;
  uint32_t orig_size, new_size;

  // Returning a a penalty of 0.0 for NULL is a GiST convention
  // This should never come about because we have decalared the
  // argument as STRICT (see gistutil.c) but we also detect NULL
  // for an invalid FPrint.
  if (orig_fp == NULL || new_fp == NULL)
  {
    if (orig_fp)
      free(orig_fp);
    if (new_fp)
      free(new_fp);

    *penalty = 1e10f;
    PG_RETURN_POINTER(penalty);
  }

  orig_size = orig_fp->max_songlen - orig_fp->min_songlen;
  new_size = (max_u32(orig_fp->max_songlen, new_songlen) - min_u32(orig_fp->min_songlen, new_songlen));
  if (new_size > 0.0f)
    songlen_diff = (float)(new_size - orig_size) / (float)new_size * 2000.0f;

  match = match_fprint_merge(new_fp, orig_fp);
  if (match > 0.0f)
  {
    match = (1.0f - match) * 100.0f;
  }
  else
  {
    match = 100.0f;
  }
  *penalty = match + songlen_diff;

  if (orig_fp)
    free(orig_fp);
  if (new_fp)
    free(new_fp);

  PG_RETURN_POINTER(penalty);
}

// Should always be untoasted Values here (see fprint_union)
// operates on result of fprint_union
Datum fprint_same(PG_FUNCTION_ARGS)
{
  fprint_gist *g0 = GET_GFP_ARG(0);
  FPrintUnion *key_fp1 = (FPrintUnion *)SERIALIZED_FP(g0);
  fprint_gist *g1 = (fprint_gist *)PG_GETARG_POINTER(1);
  FPrint *leaf_key_fp2 = SERIALIZED_FP(g1);
  bool *res = (bool *)PG_GETARG_POINTER(2);

  //*res = (bool)FP_ISMATCH(match_fprint_merge(leaf_key_fp2, key_fp1));
  *res = false;
  if (leaf_key_fp2->cprint_len == key_fp1->cprint_len)
  {
    *res = (bool)memcmp(leaf_key_fp2, key_fp1,
                        CALC_FP_SIZE(key_fp1->cprint_len));
  }

  // DO NOT free key_fp2 here (it is used elsewhere)
  PG_FREE_IF_COPY(g0, 0);

  PG_RETURN_POINTER(res);
}

/* Index Method Strategies:
 * http://www.postgresql.org/docs/8.4/interactive/xindex.html#XINDEX-BTREE-STRAT-TABLE
 *
 *   +-----------------------+-----------------------+
 *   | Operation             | Strategy Number       |
 *   +-----------------------+-----------------------+
 *   | less than             | 1                     |
 *   +-----------------------+-----------------------+
 *   | less than or equal    | 2                     |
 *   +-----------------------+-----------------------+
 *   | equal to              | 3                     |
 *   +-----------------------+-----------------------+
 *   | greater than or equal | 4                     |
 *   +-----------------------+-----------------------+
 *   | greater than          | 5                     |
 *   +-----------------------+-----------------------+
 */

/* Consistent
 * ----------
 * This function is called in several places, most notably during search.
 * It returns TRUE if the predicate is satisfiable and FALSE if not.
 *
 * StrategyNumbers are determined by the CREATE OPERATOR CLASS command;
 * see sql/pgfprint.sql
 *
 * Arguments:
 * 0 --> entry from index, passed through _decompress: untoasted
 * 1 --> query, possibly from index: possibly toasted
 * 2 --> StrategyNumber (need to implement these for joins, etc)
 * 3 --> Oid Subtype (ignored)
 * 4 --> recheck passed in; set to `true` to check lower branches and leaves
 *       Default passed in is `true`.
 *
 * NOTE: the documentation is inconsistent with regard to `recheck`;
 *       in fact, `recheck` is:
 *         - initialized to `true`
 *         - set for the scan whether the tested key is a Node or Leaf key
 *
 */
Datum fprint_consistent(PG_FUNCTION_ARGS)
{
  GISTENTRY *entry = (GISTENTRY *)PG_GETARG_POINTER(0);
  FPrint *fp = deserialize_fprint(entry->key);
  FPrintUnion *fpu = (FPrintUnion *)fp;
  FPrint *qfp = deserialize_fprint(PG_GETARG_DATUM(1));
  StrategyNumber sn = (StrategyNumber)PG_GETARG_UINT16(2);
  // arg 3 is Oid Subtype (ignored)
  // We return *recheck == true if it is an element of the index row.
  bool *recheck = PG_GETARG_POINTER(4);
  bool retval = false;
  double val = 0.0;
  double threshold = 0.08;
  float songlen_diff = 0.0f;

  if (fp == NULL || qfp == NULL)
  {
    if (fp)
      free(fp);
    if (qfp)
      free(qfp);
    *recheck = false;
    PG_RETURN_BOOL(retval);
  }

  if (GIST_LEAF(entry))
  {
    val = match_cpfm(qfp, fp);
    FPDEBUG_M("match_cpfm: %.8f", val);
    switch (sn)
    {
    case FPStrategySame:
      retval = (bool)FP_ISMATCH(val);
      break;
    case FPStrategyEQ:
      retval = (bool)FP_ISEQ(val);
      break;
    case FPStrategyNEQ:
      retval = (bool)FP_ISNEQ(val);
      break;
    default:
      retval = (bool)FP_ISMATCH(val);
      break;
    }
    *recheck = false;
    goto consistent_cleanup;
  }

  // do not set recheck to false if retval is false
  *recheck = true;
  // Tanimoto check is about ~860 ms per search
  // retval = (bool)(match_merges(qfp, fp) > 0.3);
  // The songlen setup below is the fastest yet: < 80ms, mean ~40ms.
  // Perhaps it is the picksplit algorithm or the disjunction between songlen
  // and matches but the GiST index seems to mix up when matching an entry
  // to a union at the low extreme (though 160s could hardly be considered an
  // "extreme" any more than 130s; 5s would be extreme).
  if (fpu->min_songlen <= qfp->songlen && qfp->songlen <= fpu->max_songlen)
  {
    if (qfp->songlen > 150)
    {
      threshold = 0.1;
    }
    else if (qfp->songlen > 40 && qfp->songlen < 46)
    {
      threshold = 0.03;
    }
    val = (double)match_fprint_merge(qfp, fpu);
    FPDEBUG_M("match_fprint_merge: %.16f", val);
    retval = (bool)(val > threshold);
  }
  else if (qfp->songlen < 155)
  {
    if (qfp->songlen < fpu->min_songlen)
      songlen_diff = ((float)(fpu->min_songlen - qfp->songlen) / (float)fpu->min_songlen);
    else
      songlen_diff = ((float)(qfp->songlen - fpu->max_songlen) / (float)qfp->songlen);
    if (qfp->songlen < 61)
    {
      if ((qfp->songlen < 30 && songlen_diff < .8f) ||
          (qfp->songlen < 61 && songlen_diff < .6f))
      {
        val = (double)match_fprint_merge(qfp, fpu);
        retval = (bool)(val > threshold);
      }
    }
    else if ((qfp->songlen < 110 && songlen_diff < .07f) ||
             (qfp->songlen < 155 && songlen_diff < .05f))
    {
      if (qfp->songlen > 150)
        threshold = 0.15;
      val = (double)match_fprint_merge(qfp, fpu);
      retval = (bool)(val > threshold);
    }
  }
  if (!retval)
    *recheck = false;

consistent_cleanup:
  if (fp)
    free(fp);
  if (qfp)
    free(qfp);

  PG_RETURN_BOOL(retval);
}

////////////////////////////////////////////////////////////
// Operator functions
////////////////////////////////////////////////////////////

Datum fprint_cmp(PG_FUNCTION_ARGS)
{
  fprint_gist *g0 = GET_GFP_ARG(0);
  fprint_gist *g1 = GET_GFP_ARG(1);
  FPrint *fp1 = SERIALIZED_FP(g0);
  FPrint *fp2 = SERIALIZED_FP(g1);
  double res = 0.0;

  res = match_cpfm(fp1, fp2);

  PG_FREE_IF_COPY(g0, 0);
  PG_FREE_IF_COPY(g1, 1);

  PG_RETURN_FLOAT8(res);
}

// Higher degree of certainty;
// at .98 on our match system this is practically 100%
Datum fprint_eq(PG_FUNCTION_ARGS)
{
  fprint_gist *g0 = GET_GFP_ARG(0);
  fprint_gist *g1 = GET_GFP_ARG(1);
  FPrint *fp1 = SERIALIZED_FP(g0);
  FPrint *fp2 = SERIALIZED_FP(g1);
  double val = 0.0;

  val = match_cpfm(fp1, fp2);

  PG_FREE_IF_COPY(g0, 0);
  PG_FREE_IF_COPY(g1, 1);

  PG_RETURN_BOOL((bool)FP_ISEQ(val));
}

// support <>
Datum fprint_neq(PG_FUNCTION_ARGS)
{
  fprint_gist *g0 = GET_GFP_ARG(0);
  fprint_gist *g1 = GET_GFP_ARG(1);
  FPrint *fp1 = SERIALIZED_FP(g0);
  FPrint *fp2 = SERIALIZED_FP(g1);
  double val = 0.0;

  val = match_cpfm(fp1, fp2);

  PG_FREE_IF_COPY(g0, 0);
  PG_FREE_IF_COPY(g1, 1);

  PG_RETURN_BOOL((bool)FP_ISNEQ(val));
}

// Probabilistic match based on our system,
// determined by fplib.h FP_ISMATCH.
Datum fprint_match(PG_FUNCTION_ARGS)
{
  fprint_gist *g0 = GET_GFP_ARG(0);
  fprint_gist *g1 = GET_GFP_ARG(1);
  FPrint *fp1 = SERIALIZED_FP(g0);
  FPrint *fp2 = SERIALIZED_FP(g1);
  double val = 0.0;

  val = match_cpfm(fp1, fp2);

  PG_FREE_IF_COPY(g0, 0);
  PG_FREE_IF_COPY(g1, 1);

  PG_RETURN_BOOL((bool)FP_ISMATCH(val));
}

/*  Extra functionality for fprint types
 */

#define FPRINT_ATTR_FUNC(func_name, atype, attr, pg_ret_type) \
  Datum func_name(PG_FUNCTION_ARGS);                          \
  PG_FUNCTION_INFO_V1(func_name);                             \
                                                              \
  Datum func_name(PG_FUNCTION_ARGS)                           \
  {                                                           \
    fprint_gist *gfp = GET_GFP_ARG(0);                        \
    FPrint *fp = SERIALIZED_FP(gfp);                          \
    atype attr = fp->attr;                                    \
                                                              \
    PG_FREE_IF_COPY(gfp, 0);                                  \
                                                              \
    pg_ret_type(attr);                                        \
  }

FPRINT_ATTR_FUNC(fprint_songlen, uint32_t, songlen, PG_RETURN_INT32)
FPRINT_ATTR_FUNC(fprint_num_errors, int32_t, num_errors, PG_RETURN_INT32)
