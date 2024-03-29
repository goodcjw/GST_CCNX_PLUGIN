/*
 * Copyright (C) 2012 Jiwen Cai <jwcai@cs.ucla.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccn/ccn.h>
#include <ccn/uri.h>
#include <ccn/charbuf.h>
#include <ccn/bloom.h>
#include "gstCCNxUtils.h"

static int name_compare (const void *a, const void *b);

gint32
gst_ccnx_utils_hexit (int c)
{
  if ('0' <= c && c <= '9')
    return(c - '0');
  if ('A' <= c && c <= 'F')
    return(c - 'A' + 10);
  if ('a' <= c && c <= 'f')
    return(c - 'a' + 10);
  return -1;
}

gboolean
gst_ccnx_unpack_be_guint (void* ret_int, const struct ccn_charbuf* seg)
{
  unsigned char * raw_bytes = (unsigned char *) ret_int;
  size_t i;

  for (i = 0; i < seg->length; i++) {
    raw_bytes[i] = seg->buf[seg->length - i - 1];
  }

  return TRUE;
}

gboolean
gst_ccnx_unpack_be_guint_x (void* ret_int, const unsigned char* seg, size_t len)
{
  unsigned char * raw_bytes = (unsigned char *) ret_int;
  size_t i;

  for (i = 0; i < len; i++) {
    raw_bytes[i] = seg[len - i - 1];
  }

  return TRUE;
}

struct ccn_charbuf * 
gst_ccnx_utils_get_content (
    const unsigned char * buf, ContentObject *pco)
{
  int r;
  struct ccn_charbuf * resBuffer = NULL;
  const unsigned char * tmpBuffer = NULL;
  size_t tmpBufferSize;

  r = ccn_ref_tagged_BLOB (CCN_DTAG_Content, buf,
                           pco->offset[CCN_PCO_B_Content],
                           pco->offset[CCN_PCO_E_Content],
                           &tmpBuffer,
                           &tmpBufferSize);

  if (r < 0) {
    return NULL;
  }

  /*
   * ccn_ref_tagged_BLOB get fetch some data from ccnb, it is not new data,
   * so we don't need to free tmpBuffer. but we shall free the return value
   */
  resBuffer = ccn_charbuf_create_n (tmpBufferSize);
  memcpy (resBuffer->buf, tmpBuffer, tmpBufferSize);
  resBuffer->length = tmpBufferSize;

  return resBuffer;
}

gint32
gst_ccnx_utils_get_timestamp (
    const unsigned char * content, ContentObject *pco)
{
  if (pco->offset[CCN_PCO_E_Timestamp] > pco->offset[CCN_PCO_B_Timestamp])
  {
    double dt = 0.0;
    const unsigned char *blob = NULL;
    size_t blob_size;
    size_t i;
    int secs;
    
    ccn_ref_tagged_BLOB(CCN_DTAG_Timestamp, content,
        pco->offset[CCN_PCO_B_Timestamp],
        pco->offset[CCN_PCO_E_Timestamp],
        &blob, &blob_size);
    for (i = 0; i < blob_size; i++)
      dt = dt * 256.0 + (double) blob[i];
    dt /= 4096.0;
    secs = dt; // truncates
    return secs;
  }
  else
    return -1;
}

void
gst_ccnx_utils_append_exclude_any (struct ccn_charbuf *c)
{
  unsigned char bf_all[9] = {3, 1, 'A', 0, 0, 0, 0, 0, 0xFF};
  const struct ccn_bloom_wire *b =
      ccn_bloom_validate_wire (bf_all, sizeof(bf_all));
  if (b != NULL) {
    ccn_charbuf_append_tt (c, CCN_DTAG_Bloom, CCN_DTAG);
    ccn_charbuf_append_tt (c, sizeof(bf_all), CCN_BLOB);
    ccn_charbuf_append (c, bf_all, sizeof(bf_all));
    ccn_charbuf_append_closer (c);
  }
}

struct ccn_charbuf *
gst_ccnx_utils_get_interest_name (
    const unsigned char *content, struct ccn_parsed_interest *pi)
{
  size_t start = pi->offset[CCN_PI_B_Name];
  size_t end = pi->offset[CCN_PI_E_Name];
  struct ccn_charbuf *tmpName = ccn_charbuf_create ();
  ccn_uri_append (tmpName, &content[start], end - start, 1);
  return tmpName;
}

struct ccn_charbuf * 
gst_ccnx_utils_get_content_name (const unsigned char *content, ContentObject *pco)
{
  size_t start = pco->offset[CCN_PCO_B_Name];
  size_t end = pco->offset[CCN_PCO_E_Name];
  struct ccn_charbuf *tmpName = ccn_charbuf_create ();
  ccn_uri_append (tmpName, &content[start], end - start, 1);
  return tmpName;
}

struct ccn_charbuf * 
gst_ccnx_utils_get_last_comp_from_name (const struct ccn_charbuf *name)
{
  size_t last;
  struct ccn_charbuf *tmpLast = ccn_charbuf_create ();
  for (last = name->length - 1; name->buf[last] != '/'; last--);
  last++;
  ccn_charbuf_append (tmpLast, &name->buf[last], name->length - last);
  return tmpLast;
}

void
gst_ccnx_utils_get_current_time (gdouble *now)
{
  GTimeVal nowVal;
  g_get_current_time (&nowVal);
  *now = nowVal.tv_sec + (gdouble) nowVal.tv_usec / 1000000;
}

struct ccn_charbuf *
gst_ccnx_utils_interest_prepare (GstCCNxInterestTempl * templ)
{
  struct ccn_charbuf *intBuff = NULL;
  const struct ccn_charbuf *comp = NULL;
  guint32 lifetime_l12;
  size_t i;

  intBuff = ccn_charbuf_create ();
  /* <Interest>           */
  ccn_charbuf_append_tt (intBuff, CCN_DTAG_Interest, CCN_DTAG);

  /* <Name>               */
  ccn_charbuf_append_tt (intBuff, CCN_DTAG_Name, CCN_DTAG);
  ccn_charbuf_append_closer (intBuff);
  /* </Name>              */

  if (templ->mExclList != NULL) {
    /* <Exclude>            */
    ccn_charbuf_append_tt (intBuff, CCN_DTAG_Exclude, CCN_DTAG);
    /* exclude lower side interests */
    if (templ->mExclLow)
      gst_ccnx_utils_append_exclude_any (intBuff);
    /* write down all names listed in the mExclList */
    for (i = 0; i < templ->mExclNum; i++) {
      comp = templ->mExclList[i];
      if (comp != NULL)
        ccn_charbuf_append (intBuff, comp->buf + 1, comp->length - 2);
    }
    /* exclude higher side interests */
    if (templ->mExclHigh)
      gst_ccnx_utils_append_exclude_any (intBuff);
    ccn_charbuf_append_closer (intBuff);
    /* </Exclude>           */
  }

  if (templ->mChildRight) {
    /* <ChildSelector>      */
    ccnb_tagged_putf(intBuff, CCN_DTAG_ChildSelector, "1");
    /* </ChildSelector>     */
  }

  /* <AnswerOriginKind>   */
  ccn_charbuf_append_tt(intBuff, CCN_DTAG_AnswerOriginKind, CCN_DTAG);
  ccnb_append_number(intBuff, templ->mAOK);
  ccn_charbuf_append_closer(intBuff);
  /* </AnswerOriginKind>  */

  if (templ->mLifetime > 0) {
    /* <Lifetime>           */
    unsigned char buf[3] = { 0 };
    lifetime_l12 = templ->mLifetime;
    for (i = sizeof(buf); i > 0; i--, lifetime_l12 >>= 8)
      buf[i-1] = lifetime_l12 & 0xff;
    ccnb_append_tagged_blob (intBuff, CCN_DTAG_InterestLifetime, buf, sizeof(buf));
    /* </Lifetime>          */
  }
  ccn_charbuf_append_closer (intBuff);
  /* </Interest>          */

  return intBuff;
}

void
gst_ccnx_utils_interest_destroy (GstCCNxInterestTempl **obj)
{
  size_t i;
  GstCCNxInterestTempl *templ = *obj;

  if (templ != NULL) {
    /* destroy the exclusion list and charbuf inside it */
    if (templ->mExclList != NULL) {
      for (i = 0; i < templ->mExclNum; i++)
        ccn_charbuf_destroy (&templ->mExclList[i]);
      free (templ->mExclList);
    } 
    /* destroy the object itself */
    free (templ);
    *obj = NULL;
  }
}

void
gst_ccnx_utils_interest_exclude_comp (
    GstCCNxInterestTempl *templ, struct ccn_charbuf *comp)
{
  if (templ->mExclList == NULL) {
    templ->mExclList = (struct ccn_charbuf **) calloc (
        GST_CCNX_DEFAULT_EXCL_LIST_CAP, sizeof (struct ccn_charbuf*));
    templ->mExclCap = GST_CCNX_DEFAULT_EXCL_LIST_CAP;
  }

  if (templ->mExclNum == templ->mExclCap) {
    templ->mExclCap *= 2;
    templ->mExclList = 
        (struct ccn_charbuf **) realloc (templ->mExclList, templ->mExclCap);
  }

  /* insert the comp into the exclusion list */
  templ->mExclList[templ->mExclNum++] = comp;

  /* sort the exclusion list, necessary for CCNx */
  qsort (templ->mExclList, templ->mExclNum, 
         sizeof (struct ccn_charbuf *), name_compare);
}

static int /* for qsort */
name_compare (const void *a, const void *b)
{
  const struct ccn_charbuf *aa = *(const struct ccn_charbuf **)a;
  const struct ccn_charbuf *bb = *(const struct ccn_charbuf **)b;
  int ans = ccn_compare_names(aa->buf, aa->length, bb->buf, bb->length);
  return ans;
}
