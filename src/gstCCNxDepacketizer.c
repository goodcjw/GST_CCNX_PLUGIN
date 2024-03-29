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

#include <math.h>
#include <string.h>

#include <ccn/uri.h>
#include <ccn/charbuf.h>

#include "gstCCNxUtils.h"
#include "gstCCNxDepacketizer.h"

GST_DEBUG_CATEGORY_STATIC (gst_ccnx_depkt_debug);
#define GST_CAT_DEFAULT gst_ccnx_depkt_debug

static void gst_ccnx_depkt_set_window (
    GstCCNxDepacketizer *obj, unsigned int window);
static void gst_ccnx_depkt_fetch_stream_info (GstCCNxDepacketizer *obj);
static void gst_ccnx_depkt_fetch_start_time (GstCCNxDepacketizer *obj);
static gint64 gst_ccnx_depkt_fetch_seek_query ( 
    GstCCNxDepacketizer *obj, guint64 *idxNum);

static void gst_ccnx_depkt_finish_ccnx_loop (GstCCNxDepacketizer *obj);
static gboolean gst_ccnx_depkt_check_duration (GstCCNxDepacketizer *obj);
static struct ccn_charbuf* gst_ccnx_depkt_get_duration (
    GstCCNxDepacketizer *obj, struct ccn_charbuf *intBuff);

// Bellow methods are called by thread
static void *gst_ccnx_depkt_run (void *obj);
static void gst_ccnx_depkt_process_cmds (GstCCNxDepacketizer *obj);
// def duration_process_result(self, kind, info):
static enum ccn_upcall_res gst_ccnx_depkt_process_duration_result (
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn_upcall_info *info);

static gboolean gst_ccnx_depkt_express_interest (
    GstCCNxDepacketizer *obj, guint64 seg);
static void gst_ccnx_depkt_process_response (
    GstCCNxDepacketizer *obj, struct ccn_charbuf *buf);
static void gst_ccnx_depkt_push_data (
    GstCCNxDepacketizer *obj, GstBuffer *buf);

static enum ccn_upcall_res gst_ccnx_depkt_handle_data (
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn_upcall_info *info);

static void gst_ccnx_depkt_num2seg (guint64 num, struct ccn_charbuf*seg);
static guint64 gst_ccnx_depkt_seg2num (const struct ccn_charbuf* seg);

static void
gst_ccnx_depkt_set_window (GstCCNxDepacketizer *obj, unsigned int window)
{
  obj->mFetchBuffer->mWindowSize = window;
}

static void
gst_ccnx_depkt_fetch_stream_info (GstCCNxDepacketizer *obj)
{
  gint r;
  struct ccn_charbuf *resBuff = NULL;   /* the raw result buffer with ccnb */
  struct ccn_charbuf *conBuff = NULL;   /* the pure content received */
  ContentObject *pcoBuff = NULL;

  /* prepare return values */
  resBuff = ccn_charbuf_create ();
  pcoBuff = (ContentObject *) malloc (sizeof(ContentObject));
  /* prepare name for stream_info */
  struct ccn_charbuf *nameStreamInfo = ccn_charbuf_create ();
  ccn_charbuf_append_charbuf (nameStreamInfo, obj->mName);
  ccn_name_from_uri (nameStreamInfo, "stream_info");

  r = ccn_get (obj->mCCNx, nameStreamInfo, NULL, GST_CCNX_CCN_GET_TIMEOUT,
               resBuff, pcoBuff, NULL, 0);
  
  if (r >= 0) {
    obj->mStartTime = gst_ccnx_utils_get_timestamp (resBuff->buf, pcoBuff);
    conBuff = gst_ccnx_utils_get_content (resBuff->buf, pcoBuff);
    obj->mCaps = gst_caps_from_string (ccn_charbuf_as_string (conBuff));
  }

  ccn_charbuf_destroy (&resBuff);
  ccn_charbuf_destroy (&nameStreamInfo);
  ccn_charbuf_destroy (&conBuff);
  free (pcoBuff);

  if (obj->mStartTime < 0)
    gst_ccnx_depkt_fetch_start_time (obj);
}

static void
gst_ccnx_depkt_fetch_start_time (GstCCNxDepacketizer *obj)
{
  gint r;
  /* prepare return values */
  struct ccn_charbuf *resBuff = ccn_charbuf_create ();
  ContentObject *pcoBuff = (ContentObject *) malloc (sizeof(ContentObject));
  /* prepare name for start time */
  struct ccn_charbuf *nameSegmentZero = ccn_charbuf_create ();
  ccn_charbuf_append_charbuf (nameSegmentZero, obj->mName);
  ccn_name_from_uri (nameSegmentZero, "segments");
  ccn_name_from_uri (nameSegmentZero, "%%00");

  r = ccn_get (obj->mCCNx, nameSegmentZero, NULL, GST_CCNX_CCN_GET_TIMEOUT,
               resBuff, pcoBuff, NULL, 0);

  if (r >= 0)
    obj->mStartTime = gst_ccnx_utils_get_timestamp (resBuff->buf, pcoBuff);
}

static gint64
gst_ccnx_depkt_fetch_seek_query (GstCCNxDepacketizer *obj, guint64 *idxNum)
{
  gint r;
  guint64 segNum;
  struct ccn_charbuf *idxName = NULL;
  struct ccn_charbuf *comp = NULL;  /* used to represent an entry to exclude */
  struct ccn_charbuf *intBuff = NULL;
  struct ccn_charbuf *retName = NULL;
  struct ccn_charbuf *retBuff = NULL;
  struct ccn_charbuf *conBuff = NULL;
  ContentObject *pcoBuff = NULL;
  GstCCNxInterestTempl *templ = NULL;

  templ = (GstCCNxInterestTempl *) calloc (1, sizeof (GstCCNxInterestTempl));

  /* exclusion */
  idxName = ccn_charbuf_create ();
  comp = ccn_charbuf_create ();
  gst_ccnx_depkt_num2seg (*idxNum + 1, idxName);
  ccn_name_init (comp);
  ccn_name_from_uri (comp, ccn_charbuf_as_string (idxName));
  gst_ccnx_utils_interest_exclude_comp (templ, comp);
  /* comp will be destroyed when templ being destroyed */
  ccn_charbuf_destroy (&idxName);

  templ->mChildRight = TRUE;
  templ->mAOK = 0;

  intBuff = gst_ccnx_utils_interest_prepare (templ);
  gst_ccnx_utils_interest_destroy (&templ);

  /* prepare return bufferes */
  retBuff = ccn_charbuf_create ();
  pcoBuff = (ContentObject *) malloc (sizeof (ContentObject));

  while (TRUE) {
    /* FIXME: daddy, i want the candy, or i'll never give up asking */
    r = ccn_get (obj->mCCNx, obj->mNameFrames, intBuff, GST_CCNX_CCN_GET_TIMEOUT,
                 retBuff, pcoBuff, NULL, 0);
    if (r >= 0)
      break;
  }

  /* get the real index returned with content */
  retName = gst_ccnx_utils_get_content_name (retBuff->buf, pcoBuff);
  idxName = gst_ccnx_utils_get_last_comp_from_name (retName);

  /* get the content of this packet as segNum */
  conBuff = gst_ccnx_utils_get_content (retBuff->buf, pcoBuff);
  if (conBuff != NULL && conBuff->length > 0) {
    segNum = strtoul (ccn_charbuf_as_string (conBuff), NULL, 10);
  }

  ccn_charbuf_destroy (&intBuff);
  ccn_charbuf_destroy (&retName);
  ccn_charbuf_destroy (&idxName);
  ccn_charbuf_destroy (&retBuff);
  ccn_charbuf_destroy (&conBuff);
  if (pcoBuff != NULL)
    free (pcoBuff);
  return 0;
}

GstCaps*
gst_ccnx_depkt_get_caps (GstCCNxDepacketizer *obj)
{
  GST_DEBUG ("depkt_get_caps: ");

  if (obj->mCaps == NULL) {
    GST_DEBUG ("CALL: fetch_stream_info: ");
    gst_ccnx_depkt_fetch_stream_info (obj);
    if (obj->mCaps == NULL)
      return NULL;
  }
  GST_DEBUG ("%" GST_PTR_FORMAT, obj->mCaps);
  gst_caps_ref (obj->mCaps);
  return obj->mCaps;
}

static void
gst_ccnx_depkt_finish_ccnx_loop (GstCCNxDepacketizer *obj)
{
  ccn_set_run_timeout (obj->mCCNx, 0);
}

static gboolean
gst_ccnx_depkt_check_duration (GstCCNxDepacketizer *obj)
{
  GST_DEBUG ("check_duration");
  struct ccn_charbuf *comp = NULL;
  struct ccn_charbuf *intBuff = NULL;
  struct ccn_closure *callback = NULL;
  GstCCNxInterestTempl *templ = NULL;
  
  templ = (GstCCNxInterestTempl *) calloc (1, sizeof (GstCCNxInterestTempl));
  templ->mChildRight = TRUE;
  if (obj->mDurationLast) {
    templ->mExclLow = TRUE;
    comp = ccn_charbuf_create ();
    ccn_name_init (comp);
    ccn_name_from_uri (comp, ccn_charbuf_as_string (obj->mDurationLast));
    gst_ccnx_utils_interest_exclude_comp (templ, comp);
    /* comp will be destroyed when templ being destroyed */
  }
  intBuff = gst_ccnx_utils_interest_prepare (templ);
  gst_ccnx_utils_interest_destroy (&templ);

  callback = (struct ccn_closure*) calloc (1, sizeof(struct ccn_closure));
  callback->data = obj;
  callback->p = &gst_ccnx_depkt_process_duration_result;
  ccn_express_interest (obj->mCCNx, obj->mNameFrames, callback, intBuff);

  ccn_charbuf_destroy (&intBuff);
  GST_DEBUG ("check_duration ... DONE");
  return TRUE;
}

static struct ccn_charbuf* gst_ccnx_depkt_get_duration (
    GstCCNxDepacketizer *obj, struct ccn_charbuf *intBuff)
{
  gdouble tStart, tEnd;
  struct ccn_charbuf *resBuff;
  struct ccn_charbuf *conName;
  struct ccn_charbuf *idxName;
  ContentObject *pcoBuff = NULL;
  
  gst_ccnx_utils_get_current_time (&tStart);

  /* prepare return values */
  resBuff = ccn_charbuf_create ();
  pcoBuff = (ContentObject *) malloc (sizeof(ContentObject));
  ccn_get (obj->mCCNx, obj->mNameFrames, intBuff, GST_CCNX_CCN_GET_TIMEOUT,
           resBuff, pcoBuff, NULL, 0);
  gst_ccnx_utils_get_current_time (&tEnd);

  if (resBuff == NULL)
    return NULL;

  /* get the real index returned with content */
  conName = gst_ccnx_utils_get_content_name (resBuff->buf, pcoBuff);
  idxName = gst_ccnx_utils_get_last_comp_from_name (conName);

  free (pcoBuff);
  ccn_charbuf_destroy (&resBuff);  
  ccn_charbuf_destroy (&conName);

  return idxName;
}

static void*
gst_ccnx_depkt_run (void *arg)
{
  GST_INFO ("depkt_run started");
  GstCCNxDepacketizer *obj = (GstCCNxDepacketizer *) arg;
  while (obj->mRunning) {
    gst_ccnx_depkt_check_duration (obj);
    ccn_run (obj->mCCNx, 1000);
    gst_ccnx_depkt_process_cmds (obj);
  }
  GST_INFO ("depkt_run stopped");  
  return NULL;
}

static void
gst_ccnx_depkt_process_cmds (GstCCNxDepacketizer *obj)
{
  GST_DEBUG ("process_cmds");
  guint64 idxNum = 0;
  guint64 segNum = 0;
  GstCCNxCmdsQueueEntry *cmds_entry = NULL;

  if (g_queue_is_empty (obj->mCmdsQueue)) {
    GST_DEBUG ("process_cmds ... empty mCmdsQueue");
    return;
  }
  
  cmds_entry = (GstCCNxCmdsQueueEntry *) g_queue_pop_tail (obj->mCmdsQueue);

  if (cmds_entry->mState == GST_CMD_SEEK) {
    idxNum = cmds_entry->mTimestamp;
    segNum = gst_ccnx_depkt_fetch_seek_query (obj, &idxNum);
    obj->mSeekSegment = TRUE;
    gst_ccnx_segmenter_pkt_lost (obj->mSegmenter);
    gst_ccnx_fb_reset (obj->mFetchBuffer, segNum);
    // FIXME _cmd_q.task_done ()
  }
  GST_INFO ("process_cmds ... DONE");
}

static enum ccn_upcall_res
gst_ccnx_depkt_process_duration_result (
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn_upcall_info *info)
{
  GST_DEBUG ("process_duration_result");
  GstCCNxDepacketizer *obj = (GstCCNxDepacketizer *) selfp->data;
  struct ccn_charbuf *name;

  if (kind == CCN_UPCALL_FINAL)
    return CCN_UPCALL_RESULT_OK;
  if (kind == CCN_UPCALL_CONTENT_UNVERIFIED)
    return CCN_UPCALL_RESULT_VERIFY;
  if (kind != CCN_UPCALL_CONTENT && kind != CCN_UPCALL_INTEREST_TIMED_OUT)
    return CCN_UPCALL_RESULT_ERR;

  if (kind == CCN_UPCALL_CONTENT) {
    GST_DEBUG ("CCN_UPCALL_CONTENT");
    /* get the name of returned content and store its last component */
    name = gst_ccnx_utils_get_content_name (info->content_ccnb, info->pco);
    ccn_charbuf_destroy (&obj->mDurationLast);  /* replace the exisiting one */
    obj->mDurationLast = gst_ccnx_utils_get_last_comp_from_name (name);
    GST_DEBUG ("mDurationLast: %s", ccn_charbuf_as_string (obj->mDurationLast));
    ccn_charbuf_destroy (&name);
  }
  else {
    GST_WARNING ("No response received for duration request");
  }

  if (obj->mDurationLast != NULL) {
    obj->mDurationNs = gst_ccnx_depkt_seg2num (obj->mDurationLast);
    GST_DEBUG ("obj->mDurationNs: %llu", obj->mDurationNs);
  }
  else {
    obj->mDurationNs = 0;
  }

  GST_DEBUG ("process_duration_result ... DONE");
  return CCN_UPCALL_RESULT_OK;
}

/*
 * Send out interest to fetch data
 */
static gboolean
gst_ccnx_depkt_express_interest (GstCCNxDepacketizer *obj, guint64 seg)
{
  struct ccn_charbuf *name = NULL;
  struct ccn_charbuf *segName = NULL;
  struct ccn_charbuf *intBuff = NULL;
  struct ccn_closure *callback = NULL;
  GstCCNxInterestTempl *templ = NULL;

  guint64 *key = NULL;
  GstCCNxRetryEntry * entry =
      (GstCCNxRetryEntry *) malloc (sizeof(GstCCNxRetryEntry));

  name = ccn_charbuf_create ();
  ccn_charbuf_append_charbuf (name, obj->mNameSegments);

  /* build name for the outgoing interest */
  segName = ccn_charbuf_create ();
  gst_ccnx_depkt_num2seg (seg, segName);
  ccn_name_from_uri (name, ccn_charbuf_as_string (segName));

  /* build key */
  key = (guint64 *) malloc (sizeof(guint64));
  *key = seg;

  /* build entry */
  entry->mRetryCnt = obj->mRetryCnt;
  gst_ccnx_utils_get_current_time (&entry->mTimeVal);

  g_hash_table_insert (obj->mRetryTable, key, entry);
  GST_DEBUG ("insert into mRetryTable [%llu], size=%d",
             *key, g_hash_table_size (obj->mRetryTable));

  templ = (GstCCNxInterestTempl *) calloc (1, sizeof (GstCCNxInterestTempl));
  templ->mLifetime = obj->mInterestLifetime;
  GST_DEBUG ("data interest lifetime %d", templ->mLifetime / 4096);
  intBuff = gst_ccnx_utils_interest_prepare (templ);
  gst_ccnx_utils_interest_destroy (&templ);

  callback = (struct ccn_closure*) calloc (1, sizeof(struct ccn_closure));
  callback->data = obj;
  callback->p = gst_ccnx_depkt_handle_data;
  ccn_express_interest (obj->mCCNx, name, callback, intBuff);
  GST_DEBUG ("express_interest: %s", ccn_charbuf_as_string (segName));

  ccn_charbuf_destroy (&name);
  ccn_charbuf_destroy (&segName);
  ccn_charbuf_destroy (&intBuff);

  GST_DEBUG ("express_interest ... DONE");
  return TRUE;
}

/*
 * when this function returns, content will be released immediately
 */
static void
gst_ccnx_depkt_process_response (
    GstCCNxDepacketizer *obj, struct ccn_charbuf *content)
{
  GST_DEBUG ("process_response");
  if (content == NULL) {
    GST_WARNING ("process response: data lost");
    gst_ccnx_segmenter_pkt_lost (obj->mSegmenter);
    return;
  }
  // now we get the content, we don't need buf and pco anymore,
  // content as a ccn_charbuf will be freed in segmenter's queue management.
  // FIXME content might be released...
  gst_ccnx_segmenter_process_pkt (obj->mSegmenter, content);
  GST_DEBUG ("process_response ... DONE");
}

static void
gst_ccnx_depkt_push_data (GstCCNxDepacketizer *obj, GstBuffer *buf)
{
  GST_INFO ("push_data");

  GstCCNxCmd status = GST_CMD_INVALID;
  gint32 queueSize;
  GstCCNxDataQueueEntry *entry;

  if (obj->mSeekSegment == TRUE) {
    GST_WARNING ("Marking as discontinued");
    status = GST_CMD_SEEK;
    obj->mSeekSegment = FALSE;
  }

  while (TRUE) {
    /* FIXME this shall be run in a seperate thread ? */
    queueSize = g_queue_get_length (obj->mDataQueue);
    if (queueSize < obj->mWindowSize * 2) {
      entry = (GstCCNxDataQueueEntry *) malloc (sizeof(GstCCNxDataQueueEntry));
      entry->mState = status;
      entry->mData = buf;
      g_queue_push_head (obj->mDataQueue, entry);
      queueSize = g_queue_get_length (obj->mDataQueue);
      GST_INFO ("push into data queue: %p, size=%d", entry, queueSize);
      break;
    }
    else {
      /* the queue is full */
      if (obj->mRunning == FALSE)
        break;
    }
  }
  GST_INFO ("push_data ... DONE");
}

/*
 * Handle data response
 */
static enum ccn_upcall_res
gst_ccnx_depkt_handle_data (struct ccn_closure *selfp,
                            enum ccn_upcall_kind kind,
                            struct ccn_upcall_info *info)
{
  GST_DEBUG ("handle_data");
  struct ccn_charbuf *intName = NULL;    /* interest name */
  struct ccn_charbuf *conName = NULL;    /* content name */
  /* segName and segNum are used to look up as key in mRetryTable */
  struct ccn_charbuf *segName = NULL;
  struct ccn_charbuf *content = NULL;
  guint64 segNum = 0;
  guint64 *new_key;
  gdouble now, rtt, diff, absdiff;
  GstCCNxRetryEntry *entry = NULL;
  GstCCNxRetryEntry *new_entry = NULL;  

  GstCCNxDepacketizer *depkt = (GstCCNxDepacketizer *) selfp->data;

  if (depkt->mRunning != TRUE) {
    GST_WARNING ("handle_data depkt is not running");
    return CCN_UPCALL_RESULT_OK;
  }
  else if (kind == CCN_UPCALL_FINAL) {
    GST_DEBUG ("handle_data CCN_UPCALL_FINAL");
    return CCN_UPCALL_RESULT_OK;
  }
  else if (kind == CCN_UPCALL_CONTENT) {
    intName = gst_ccnx_utils_get_interest_name (info->interest_ccnb, info->pi);
    segName = gst_ccnx_utils_get_last_comp_from_name (intName);
    segNum = gst_ccnx_depkt_seg2num (segName);

    /* lookup the sending time */
    entry = (GstCCNxRetryEntry*) g_hash_table_lookup (
        depkt->mRetryTable, &segNum);

    if (entry == NULL) {
      GST_WARNING ("failed to find entry [%llu] size=%d",
                   segNum, g_hash_table_size (depkt->mRetryTable));
      return CCN_UPCALL_RESULT_ERR;  /* this should never happen */
    }

    ccn_charbuf_destroy (&intName);
    ccn_charbuf_destroy (&segName);

    gst_ccnx_utils_get_current_time (&now);
    rtt = now - entry->mTimeVal;
    diff = rtt - depkt->mSRtt;
    absdiff = (diff > 0) ? diff : -diff;
    depkt->mSRtt += 1 / 128.0 * diff;
    depkt->mRttVar += 1 / 64.0 * (absdiff - depkt->mRttVar);
    depkt->mInterestLifetime = 
        (int) (4096 * (depkt->mSRtt + 3 * sqrt (depkt->mRttVar)));
    g_hash_table_remove (depkt->mRetryTable, &segNum) ;

    /* now we buffer the content */
    conName = gst_ccnx_utils_get_interest_name (info->interest_ccnb, info->pi);
    segName = gst_ccnx_utils_get_last_comp_from_name (conName);

    content = gst_ccnx_utils_get_content (info->content_ccnb, info->pco);
    gst_ccnx_fb_put (
        depkt->mFetchBuffer, gst_ccnx_depkt_seg2num (segName), content);

    GST_DEBUG ("handle_data: data received");
    return CCN_UPCALL_RESULT_OK;
  }
  else if (kind == CCN_UPCALL_INTEREST_TIMED_OUT) {
    GST_DEBUG ("handle_data CCN_UPCALL_TIMED_OUT");
    intName = gst_ccnx_utils_get_interest_name (info->interest_ccnb, info->pi);
    segName = gst_ccnx_utils_get_last_comp_from_name (intName);
    segNum = gst_ccnx_depkt_seg2num (segName);

    /* 
     * the definition about InterestLifetime is different with Derek's, here we
     * use 1/4096 sec, while Derek is using second directly.
     */
    depkt->mInterestLifetime = 2 * 4096;

    entry = (GstCCNxRetryEntry*) g_hash_table_lookup (
        depkt->mRetryTable, &segNum);

    if (entry->mRetryCnt > 0) {
      depkt->mStatsRetries++;
      gst_ccnx_utils_get_current_time (&now);
      /* build new key */
      new_key = (guint64 *) malloc (sizeof(guint64));
      *new_key = segNum;
      /* build new entry */
      new_entry = (GstCCNxRetryEntry *) malloc (sizeof(GstCCNxRetryEntry));
      new_entry->mRetryCnt = entry->mRetryCnt;
      new_entry->mTimeVal = now;
      g_hash_table_replace (depkt->mRetryTable, new_key, new_entry);
      GST_DEBUG ("replace entry [%llu], size=%d entry=%p", 
                 *new_key, g_hash_table_size (depkt->mRetryTable), new_entry);
      return CCN_UPCALL_RESULT_REEXPRESS;
    }

    depkt->mStatsDrops++;
    g_hash_table_remove (depkt->mRetryTable, &segNum);
    gst_ccnx_fb_timeout (depkt->mFetchBuffer, gst_ccnx_depkt_seg2num (segName));

    GST_DEBUG ("handle_data: interest timed out");

    ccn_charbuf_destroy (&intName);
    ccn_charbuf_destroy (&segName);

    return CCN_UPCALL_RESULT_OK;
  }
  else if (kind == CCN_UPCALL_CONTENT_UNVERIFIED) {
    GST_DEBUG ("handle_data: unverified content");
    return CCN_UPCALL_RESULT_VERIFY;
  }
  
  GST_DEBUG ("handle_data: got unknow kind");
  return CCN_UPCALL_RESULT_ERR;
}

static guint64
gst_ccnx_depkt_seg2num (const struct ccn_charbuf *seg)
{
  size_t i;
  guint64 num = 0;

  for (i = 0; i < seg->length; i++) {
    if (seg->buf[i] == '%') {
      num = num * 256 
          + (gst_ccnx_utils_hexit(seg->buf[i+1])) * 16
          + (gst_ccnx_utils_hexit(seg->buf[i+2]));
      i += 2;
    }
    else {
      num = num * 256 + ((guint64) seg->buf[i]);
    }
  }

  return num;
}

static void
gst_ccnx_depkt_num2seg (guint64 num, struct ccn_charbuf *seg)
{
  size_t i;
  char aByte;
  struct ccn_charbuf *tmp;

  tmp = ccn_charbuf_create ();
  while (num != 0) {
    aByte = num % 256;
    num = num / 256;
    ccn_charbuf_append_value(tmp, (char) aByte, 1);
  }
  ccn_charbuf_append_value(tmp, 0, 1);
  // reverse the buffer
  for (i = 0; i < tmp->length / 2; i++) {
    aByte = tmp->buf[i];
    tmp->buf[i] = tmp->buf[tmp->length - i - 1];
    tmp->buf[tmp->length - i - 1] = aByte;
  }

  ccn_charbuf_reset (seg);
  for (i = 0; i < tmp->length; i++) {
    ccn_charbuf_putf (seg, "%%%02X", tmp->buf[i]);
  }

  ccn_charbuf_destroy (&tmp);
}

/* public methods */

GstCCNxDepacketizer *
gst_ccnx_depkt_create (
    const gchar *name, gint32 window_size, guint32 time_out, gint32 retries)
{
  GST_DEBUG_CATEGORY_INIT (gst_ccnx_depkt_debug, "GST_CCNX_DEPKT",
      2, "Receives video and audio data over a CCNx network");

  GstCCNxDepacketizer *obj =
      (GstCCNxDepacketizer *) malloc (sizeof(GstCCNxDepacketizer));
  obj->mWindowSize = window_size;
  obj->mInterestLifetime = time_out;
  obj->mRetryCnt = retries;

  obj->mDataQueue = g_queue_new ();
  obj->mDurationNs = -1;
  obj->mRunning = FALSE;
  obj->mCaps = NULL;
  obj->mStartTime = -1;
  obj->mSeekSegment = FALSE;
  obj->mDurationLast = NULL;
  obj->mCmdsQueue = g_queue_new ();

  obj->mCCNx = ccn_create ();
  if (ccn_connect (obj->mCCNx, NULL) == -1) {
    /* LOG: cannot connect to CCN */
    gst_ccnx_depkt_destroy (&obj);
    return NULL;
  }
  
  obj->mName = ccn_charbuf_create ();
  obj->mNameSegments = ccn_charbuf_create ();
  obj->mNameFrames = ccn_charbuf_create ();
  ccn_name_from_uri (obj->mName, name);
  ccn_name_from_uri (obj->mNameSegments, name);
  ccn_name_from_uri (obj->mNameSegments, "segments");
  ccn_name_from_uri (obj->mNameFrames, name);
  ccn_name_from_uri (obj->mNameFrames, "index");

  obj->mFetchBuffer = gst_ccnx_fb_create (
      obj, obj->mWindowSize,
      gst_ccnx_depkt_express_interest,
      gst_ccnx_depkt_process_response);

  obj->mSegmenter = gst_ccnx_segmenter_create (
      obj, gst_ccnx_depkt_push_data, GST_CCNX_CHUNK_SIZE);

  obj->mRetryTable = 
      g_hash_table_new_full (g_int64_hash, g_int64_equal, free, free);

  obj->mSRtt = 0.05;
  obj->mRttVar = 0.01;
  obj->mStatsRetries = 0;
  obj->mStatsDrops = 0;

  return obj;
}

void
gst_ccnx_depkt_destroy (GstCCNxDepacketizer **obj)
{
  GstCCNxDepacketizer *depkt = *obj;
  GstCCNxDataQueueEntry *data_entry;
  GstCCNxCmdsQueueEntry *cmds_entry;

  GST_DEBUG ("CALL depkt_destroy");
  if (depkt != NULL) {
    /* destroy data queue and cmds queue */
    while ((data_entry = (GstCCNxDataQueueEntry *) g_queue_pop_tail (
               depkt->mDataQueue)) != NULL) {
      gst_buffer_unref (data_entry->mData);
      free (data_entry);
    }
    while ((cmds_entry = (GstCCNxCmdsQueueEntry *) g_queue_pop_tail (
               depkt->mCmdsQueue)) != NULL)
      free (cmds_entry);

    /* destroy dynamic allocated structs */
    ccn_destroy (&depkt->mCCNx);
    // FIXME check whether this is correct
    if (depkt->mCaps != NULL)
      gst_caps_unref (depkt->mCaps);
    ccn_charbuf_destroy (&depkt->mName);
    ccn_charbuf_destroy (&depkt->mNameSegments);
    ccn_charbuf_destroy (&depkt->mNameFrames);
    ccn_charbuf_destroy (&depkt->mDurationLast);
    gst_ccnx_fb_destroy (&depkt->mFetchBuffer);
    gst_ccnx_segmenter_destroy (&depkt->mSegmenter);
    g_hash_table_destroy (depkt->mRetryTable);

    /* destroy the object itself */
    free (depkt);
    *obj = NULL;
  }
}

gboolean
gst_ccnx_depkt_start (GstCCNxDepacketizer *obj)
{
  int rc;
  rc = pthread_create (&obj->mReceiverThread, NULL,
                       gst_ccnx_depkt_run, obj);
  if (rc != 0)
    return FALSE;

  GST_INFO ("depkt_start: new thread created!");
  obj->mRunning = TRUE;
  return TRUE;
}

gboolean
gst_ccnx_depkt_stop (GstCCNxDepacketizer *obj)
{
  int rc;
  obj->mRunning = FALSE;
  gst_ccnx_depkt_finish_ccnx_loop (obj);
  rc = pthread_join (obj->mReceiverThread, NULL);
  if (rc != 0)
    return FALSE;
  return TRUE;
}

gboolean
gst_ccnx_depkt_init_duration (GstCCNxDepacketizer *obj)
{
  struct ccn_charbuf *intBuff = NULL;
  struct ccn_charbuf *retBuff = NULL;
  struct ccn_charbuf *comp = NULL;
  GstCCNxInterestTempl *templ = NULL;
  gboolean res = FALSE;

  templ = (GstCCNxInterestTempl *) calloc (1, sizeof (GstCCNxInterestTempl));
  templ->mChildRight = TRUE;
  templ->mAOK = CCN_AOK_DEFAULT;
  intBuff = gst_ccnx_utils_interest_prepare (templ);

  while (TRUE) {
    retBuff = gst_ccnx_depkt_get_duration (obj, intBuff);
    if (retBuff == NULL)
      break;
    // TODO finish this after we got the player
  }
  gst_ccnx_utils_interest_destroy (&templ);
  return res;
}

void
gst_ccnx_depkt_seek (GstCCNxDepacketizer *obj, gint64 seg_start)
{
  GST_DEBUG ("CALL %s", "depkt_seek");
  GstCCNxCmdsQueueEntry *cmds_entry =
      (GstCCNxCmdsQueueEntry *) malloc (sizeof(GstCCNxCmdsQueueEntry));
  cmds_entry->mState = GST_CMD_SEEK;
  cmds_entry->mTimestamp = seg_start;
  g_queue_push_head (obj->mCmdsQueue, cmds_entry);
  gst_ccnx_depkt_finish_ccnx_loop (obj);
}

/* test functions */
#ifdef GST_CCNX_TEST

guint64
test_pack_unpack (guint64 num) {
  struct ccn_charbuf *buf_1 = ccn_charbuf_create ();
  gst_ccnx_depkt_num2seg(num, buf_1);
  fwrite (buf_1->buf, buf_1->length, 1, stdout);
  num = gst_ccnx_depkt_seg2num (buf_1);
  printf("\t%lu", num);
  return num;
}

#endif
