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

#ifndef __GST_CCNX_UTILS_H__
#define __GST_CCNX_UTILS_H__

#include <gst/gst.h>

struct ccn_charbuf;
struct ccn_parsed_ContentOjbect;
typedef struct ccn_parsed_ContentObject ContentObject;

typedef struct _GstCCNxInterestTempl GstCCNxInterestTempl;

struct _GstCCNxInterestTempl {
  struct ccn_charbuf                **mExclList;
  size_t                              mExclCap;
  size_t                              mExclNum;
  gboolean                            mExclLow;
  gboolean                            mExclHigh;
  gboolean                            mChildRight;
  gint                                mAOK;
  guint32                             mLifetime;
};

#define GST_CCNX_DEFAULT_EXCL_LIST_CAP   4

gint32
gst_ccnx_utils_hexit (int c);

gboolean
gst_ccnx_unpack_be_guint (void* ret_int, const struct ccn_charbuf* seg);

gboolean
gst_ccnx_unpack_be_guint_x (void* ret_int, const unsigned char* seg, size_t len);

struct ccn_charbuf * 
gst_ccnx_utils_get_content (
    const unsigned char * content, ContentObject *pco);

gint32 
gst_ccnx_utils_get_timestamp (
    const unsigned char * content, ContentObject *pco);

/*
 * This appends a tagged, valid, fully-saturated Bloom filter, useful for
 * excluding everything between two 'fenceposts' in an Exclude construct.
 */
void gst_ccnx_utils_append_exclude_any (struct ccn_charbuf *c);

struct ccn_charbuf *gst_ccnx_utils_get_content_name (
    const unsigned char *content, ContentObject *pco);

struct ccn_charbuf *gst_ccnx_utils_get_interest_name (
    const unsigned char *content, struct ccn_parsed_interest *pi);

struct ccn_charbuf * 
gst_ccnx_utils_get_last_comp_from_name (const struct ccn_charbuf *name);

void gst_ccnx_utils_get_current_time (gdouble *now);

struct ccn_charbuf *
gst_ccnx_utils_interest_prepare (GstCCNxInterestTempl *templ);

void
gst_ccnx_utils_interest_destroy (GstCCNxInterestTempl **templ);

void
gst_ccnx_utils_interest_exclude_comp (
    GstCCNxInterestTempl *templ, struct ccn_charbuf *comp);

#endif // __GST_CCNX_UTILS_H__
