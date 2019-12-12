/*
* Copyright 2018 RDK Management
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Library General Public
* License as published by the Free Software Foundation, version 2
* of the license.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Library General Public License for more details.
*
* You should have received a copy of the GNU Library General Public
* License along with this library; if not, write to the
* Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
* Boston, MA 02110-1301, USA.
*/

/**
 * @file gstaamp.cpp
 * @brief AAMP gstreamer plugin definitions
 */


// shripad testing tag = fr_test_fed_gstplg_2

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <string.h>
#include <stdio.h>
#include "gstaamp.h"
#include "main_aamp.h"
#include "priv_aamp.h"

GST_DEBUG_CATEGORY_STATIC (gst_aamp_debug_category);
#define GST_CAT_DEFAULT gst_aamp_debug_category

#define MAX_BYTES_TO_SEND (188*1024)
#define MAX_NUM_BUFFERS_IN_QUEUE 30

#define  GST_AAMP_LOG_TIMING(msg...) GST_FIXME_OBJECT(aamp, msg)

static const gchar *g_aamp_expose_hls_caps = NULL;

static GstStateChangeReturn
gst_aamp_change_state(GstElement * element, GstStateChange transition);
static void gst_aamp_finalize(GObject * object);
static gboolean gst_aamp_query(GstElement * element, GstQuery * query);

static GstFlowReturn gst_aamp_sink_chain(GstPad * pad, GstObject *parent, GstBuffer * buffer);
static gboolean gst_aamp_sink_event(GstPad * pad, GstObject *parent, GstEvent * event);

static gboolean gst_aamp_src_event(GstPad * pad, GstObject *parent, GstEvent * event);
static gboolean gst_aamp_src_query(GstPad * pad, GstObject *parent, GstQuery * query);

static void gst_aamp_configure(GstAamp * aamp, StreamOutputFormat format, StreamOutputFormat audioFormat);
static gboolean gst_aamp_ready(GstAamp *aamp);

#ifdef AAMP_JSCONTROLLER_ENABLED
extern "C"
{
	void setAAMPPlayerInstance(PlayerInstanceAAMP *, int);
	void unsetAAMPPlayerInstance(PlayerInstanceAAMP *);
}
#endif

/**
 * @enum GstAampProperties
 * @brief Placeholder for gstaamp  properties
 */
enum GstAampProperties
{
	PROP_0
};

gboolean gst_aamp_push(media_stream* stream, GstMiniObject *obj, gboolean *eosEvent = NULL)
{
	gboolean retVal = TRUE;
	if (GST_IS_BUFFER(obj))
	{
		GstFlowReturn ret;
		ret = gst_pad_push(stream->srcpad, GST_BUFFER(obj));
		if (ret != GST_FLOW_OK)
		{
			GST_WARNING_OBJECT(stream->parent, "gst_pad_push[%s] error: %s \n", GST_PAD_NAME(stream->srcpad),
			        gst_flow_get_name(ret));
			gst_pad_pause_task(stream->srcpad);
			retVal = FALSE;
		}
	}
	else if (GST_IS_EVENT(obj))
	{
		GstEvent* event = GST_EVENT(obj);
		if (eosEvent)
		{
			if (GST_EVENT_TYPE(event) == GST_EVENT_EOS)
			{
				*eosEvent = TRUE;
			}
		}
		GST_INFO_OBJECT(stream->parent, "%s: send %s event\n", __FUNCTION__,
		        GST_EVENT_TYPE_NAME(event));
		if (!gst_pad_push_event(stream->srcpad, event))
		{
			GST_WARNING_OBJECT(stream->parent, "gst_pad_push_event[%s] error\n", GST_PAD_NAME(stream->srcpad));
		}
	}
	else
	{
		GST_ERROR_OBJECT(stream->parent, "%s: unexpected object type in queue\n", __FUNCTION__);
	}
	return retVal;
}

/**
 * @brief Enqueue buffer/event to queue
 * @param[in] stream Media stream object pointer
 * @param[in] item pointer to the object to be added to queue/pushed
 */
void gst_aamp_stream_add_item(media_stream* stream, gpointer item)
{
	GstAamp *aamp = GST_AAMP(stream->parent);
	g_assert(NULL != stream->srcpad);
	if (stream->parent->enable_src_tasks)
	{
		g_mutex_lock(&stream->mutex);
		while (g_queue_get_length(stream->queue) > MAX_NUM_BUFFERS_IN_QUEUE)
		{
			g_cond_wait(&stream->cond, &stream->mutex);
			if (aamp->flushing)
			{
				break;
			}
		}
		if (aamp->flushing)
		{
			GstMiniObject *obj = (GstMiniObject *) item;
			if (GST_IS_BUFFER(obj))
			{
				gst_buffer_unref(GST_BUFFER(obj));
			}
			else if (GST_IS_EVENT(obj))
			{
				gst_event_unref(GST_EVENT(obj));
			}
			else
			{
				GST_ERROR_OBJECT(aamp, "%s: unexpected object type in queue\n", __FUNCTION__);
			}
		}
		else
		{
			g_queue_push_tail(stream->queue, item);
		}
		g_cond_broadcast(&stream->cond);
		g_mutex_unlock(&stream->mutex);
	}
	else
	{
		gst_aamp_push(stream, (GstMiniObject *) item);
	}
}

/**
 * @brief Dequeue buffer/event and push it to srcpad
 * @param[in] stream Media stream object pointer
 */
void gst_aamp_stream_push_next_item(media_stream* stream)
{
	GstAamp *aamp = GST_AAMP(stream->parent);
	GST_DEBUG_OBJECT(aamp, "Enter\n");
	gboolean eosSent = FALSE;
	while (!eosSent)
	{
		g_mutex_lock(&stream->mutex);
		if (g_queue_is_empty(stream->queue) && !aamp->flushing)
		{
			g_cond_wait(&stream->cond, &stream->mutex);
		}
		if (aamp->flushing)
		{
			GST_INFO_OBJECT(aamp, "Flushing");
			g_mutex_unlock(&stream->mutex);
			gst_pad_pause_task(stream->srcpad);
			break;
		}
		gpointer item = g_queue_pop_head(stream->queue);
		g_cond_broadcast(&stream->cond);
		g_mutex_unlock(&stream->mutex);
		if (item)
		{
			if (!gst_aamp_push(stream, (GstMiniObject *)item, &eosSent))
			{
				break;
			}
		}
		else
		{
			GST_ERROR_OBJECT(aamp, "%s: object NULL\n", __FUNCTION__);
			break;
		}
	}
}

/**
 * @brief Start src pad task of stream
 * @param[in] stream Media stream object pointer
 */
void gst_aamp_stream_start(media_stream* stream)
{
	if(stream->srcpad)
	{
		GST_INFO_OBJECT(stream->parent, "start %s pad task", GST_PAD_NAME(stream->srcpad));
		gst_pad_start_task (stream->srcpad, (GstTaskFunction) gst_aamp_stream_push_next_item,
				stream, NULL);
	}
}

/**
 * @brief Flushes stream queue
 * @param[in] aamp Gstreamer aamp object pointer
 */
void gst_aamp_stream_flush(GstAamp *aamp)
{
	for (int i = 0; i < 2; i++)
	{
		media_stream* stream = &aamp->stream[i];
		if (stream->srcpad)
		{
			if (aamp->enable_src_tasks)
			{
				g_mutex_lock(&stream->mutex);
				while (FALSE == g_queue_is_empty(stream->queue))
				{
					GstMiniObject *obj = (GstMiniObject *) g_queue_pop_head(stream->queue);
					if (GST_IS_BUFFER(obj))
					{
						gst_buffer_unref(GST_BUFFER(obj));
					}
					else if (GST_IS_EVENT(obj))
					{
						gst_event_unref(GST_EVENT(obj));
					}
					else
					{
						GST_ERROR_OBJECT(stream->parent, "%s: unexpected object type in queue\n", __FUNCTION__);
					}
				}
				g_cond_broadcast(&stream->cond);
				g_mutex_unlock(&stream->mutex);
			}
		}
	}
}

/**
 * @brief Flushes gstreamer elements and stop any running pad tasks
 * @param[in] aamp Gstreamer aamp object pointer
 */
void gst_aamp_stop_and_flush(GstAamp *aamp)
{
	aamp->flushing = TRUE;
	gst_aamp_stream_flush(aamp);
	for (int i = 0; i < 2; i++)
	{
		media_stream* stream = &aamp->stream[i];
		if (stream->srcpad)
		{
			GST_INFO_OBJECT(aamp, "[%s]sending flush start", GST_PAD_NAME(stream->srcpad));
			gst_pad_push_event(stream->srcpad, gst_event_new_flush_start());
			if (aamp->enable_src_tasks)
			{
				gst_pad_stop_task(stream->srcpad);
			}
			GST_INFO_OBJECT(aamp, "[%s]sending flush stop", GST_PAD_NAME(stream->srcpad));
			gst_pad_push_event(stream->srcpad, gst_event_new_flush_stop(TRUE));
		}
	}
	aamp->flushing = FALSE;
}

/**
 * @class GstAampStreamer
 * @brief Handle media data/configuration/events from AAMP core
 */
class GstAampStreamer : public StreamSink, public AAMPEventListener
{
public:
	/**
	 * @brief GstAampStreamer Constructor
	 * @param[in] aamp Associated gstaamp pointer
	 */
	GstAampStreamer(GstAamp * aamp)
	{
		this->aamp = aamp;
		rate = AAMP_NORMAL_PLAY_RATE;
		srcPadCapsSent = true;
		format = FORMAT_INVALID;
		audioFormat = FORMAT_NONE;
		readyToSend = false;
	}


	/**
	 * @brief Configures gstaamp with stream output formats
	 * @param[in] format Output format of main media
	 * @param[in] audioFormat Output format of audio if present
	 */
	void Configure(StreamOutputFormat format, StreamOutputFormat audioFormat, bool bESChangeStatus)
	{
		GST_INFO_OBJECT(aamp, "Enter format = %d audioFormat = %d", format, audioFormat);
		this->format = format;
		this->audioFormat = audioFormat;
		gst_aamp_configure(aamp, format, audioFormat);
	}


	/**
	 * @brief Sends pending events to stream's src pad
	 * @param[in] stream Media stream to which events are sent
	 * @param[in] pts Presentation time-stamp
	 */
	void SendPendingEvents(media_stream* stream, GstClockTime pts)
	{
		if (stream->streamStart)
		{
			GST_INFO_OBJECT(aamp, "sending new_stream_start\n");
			gst_aamp_stream_add_item(stream, gst_event_new_stream_start(aamp->stream_id));

			GST_INFO_OBJECT(aamp, "%s: sending caps1\n", __FUNCTION__);
			gst_aamp_stream_add_item(stream, gst_event_new_caps(stream->caps));
			stream->streamStart = FALSE;
			GST_INFO_OBJECT(aamp, "%s: sent caps\n", __FUNCTION__);
		}
		if (stream->flush)
		{
			GST_INFO_OBJECT(aamp, "%s: sending flush start\n", __FUNCTION__);
			gst_aamp_stream_add_item(stream, gst_event_new_flush_start());

#ifdef USE_GST1
			GstEvent* event = gst_event_new_flush_stop(FALSE);
#else
			GstEvent* event = gst_event_new_flush_stop();
#endif
			GST_INFO_OBJECT(aamp, "%s: sending flush stop\n", __FUNCTION__);
			gst_aamp_stream_add_item(stream, event);
			stream->flush = FALSE;
		}
		if (stream->resetPosition)
		{
#ifdef USE_GST1
			GstSegment segment;
			gst_segment_init(&segment, GST_FORMAT_TIME);
			segment.start = pts;
			segment.position = 0;
			segment.rate = AAMP_NORMAL_PLAY_RATE;
			segment.applied_rate = rate;
			GST_INFO_OBJECT(aamp, "Sending segment event. start %" G_GUINT64_FORMAT " stop %" G_GUINT64_FORMAT" rate %f\n", segment.start, segment.stop, segment.rate);
			GstEvent* event = gst_event_new_segment (&segment);
#else
			GstEvent* event = gst_event_new_new_segment(FALSE, AAMP_NORMAL_PLAY_RATE, GST_FORMAT_TIME, pts, GST_CLOCK_TIME_NONE, 0);
#endif
			GST_INFO_OBJECT(aamp, "%s: sending segment\n", __FUNCTION__);
			gst_aamp_stream_add_item(stream, event);
			stream->resetPosition = FALSE;
		}
		stream->eventsPending = FALSE;
	}


	/**
	 * @brief Sends stream data to src pad
	 * @param[in] mediaType MediaType of data
	 * @param[in] ptr Data buffer
	 * @param[in] len0 Size of data buffer
	 * @param[in] fpts PTS of buffer in seconds
	 * @param[in] fdts DTS of buffer in seconds
	 * @param[in] fDuration Duration of buffer in seconds
	 * @note Caller owns ptr, may free on return
	 */
	void Send(MediaType mediaType, const void *ptr, size_t len0, double fpts, double fdts, double fDuration)
	{
		gboolean discontinuity = FALSE;

#ifdef AAMP_DISCARD_AUDIO_TRACK
		if (mediaType == eMEDIATYPE_AUDIO)
		{
			GST_WARNING_OBJECT(aamp, "Discard audio track- not sending data\n");
			return;
		}
#endif

		const char* mediaTypeStr = (mediaType==eMEDIATYPE_AUDIO)?"eMEDIATYPE_AUDIO":"eMEDIATYPE_VIDEO";
		GST_DEBUG_OBJECT(aamp, "Enter len = %d fpts %f mediaType %s", (int)len0, fpts, mediaTypeStr);
		if (!readyToSend)
		{
			if (!gst_aamp_ready(aamp))
			{
				GST_WARNING_OBJECT(aamp, "Not ready to consume data type %s\n", mediaTypeStr);
				return;
			}
			readyToSend = true;
		}
		media_stream* stream = &aamp->stream[mediaType];

		GstClockTime pts = (GstClockTime)(fpts * GST_SECOND);
		GstClockTime dts = (GstClockTime)(fdts * GST_SECOND);
//#define TRACE_PTS_TRACK 0xff
#ifdef TRACE_PTS_TRACK
		if (( mediaType == TRACE_PTS_TRACK ) || (0xFF == TRACE_PTS_TRACK))
		{
			printf("%s : fpts %f pts %llu", (mediaType == eMEDIATYPE_VIDEO)?"vid":"aud", fpts, (unsigned long long)pts);
			GstClock *clock = GST_ELEMENT_CLOCK(aamp);
			if (clock)
			{
				GstClockTime curr = gst_clock_get_time(clock);
				printf(" provided clock time %lu diff (pts - curr) %lu (%lu ms)", (unsigned long)curr, (unsigned long)(pts-curr), (unsigned long)(pts-curr)/GST_MSECOND);
			}
			printf("\n");
		}
#endif

		if(stream->eventsPending)
		{
			SendPendingEvents(stream , pts);
			discontinuity = TRUE;

		}
#ifdef GSTAAMP_DUMP_STREAM
		static FILE* fp[2] = {NULL,NULL};
		static char filename[128];
		if (!fp[mediaType])
		{
			sprintf(filename, "gstaampdump%d.ts",mediaType );
			fp[mediaType] = fopen(filename, "w");
		}
		fwrite(ptr, 1, len0, fp[mediaType] );
#endif

		while (aamp->player_aamp->aamp->DownloadsAreEnabled())
		{
			size_t len = len0;
			if (len > MAX_BYTES_TO_SEND)
			{
				len = MAX_BYTES_TO_SEND;
			}
#ifdef USE_GST1
			GstBuffer *buffer = gst_buffer_new_allocate(NULL, (gsize) len, NULL);
			GstMapInfo map;
			gst_buffer_map(buffer, &map, GST_MAP_WRITE);
			memcpy(map.data, ptr, len);
			gst_buffer_unmap(buffer, &map);
			GST_BUFFER_PTS(buffer) = pts;
			GST_BUFFER_DTS(buffer) = dts;
#else
			GstBuffer *buffer = gst_buffer_new_and_alloc((guint) len);
			memcpy(GST_BUFFER_DATA(buffer), ptr, len);
			GST_BUFFER_TIMESTAMP(buffer) = pts;
#endif
			if (discontinuity)
			{
				GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
				discontinuity = FALSE;
			}

			gst_aamp_stream_add_item( stream, buffer);
			ptr = len + (unsigned char *) ptr;
			len0 -= len;
			if (len0 == 0)
			{
				break;
			}
		}
		GST_TRACE_OBJECT(aamp, "Exit");
	}


	/**
	 * @brief Sends stream data to src pad
	 * @param[in] mediaType MediaType of data
	 * @param[in] pBuffer Data buffer
	 * @param[in] fpts PTS of buffer in seconds
	 * @param[in] fdts DTS of buffer in seconds
	 * @param[in] fDuration Duration of buffer in seconds
	 * @note Ownership of pBuffer is transferred
	 */
	void Send(MediaType mediaType, GrowableBuffer* pBuffer, double fpts, double fdts, double fDuration)
	{
		gboolean discontinuity = FALSE;

#ifdef AAMP_DISCARD_AUDIO_TRACK
		if (mediaType == eMEDIATYPE_AUDIO)
		{
			GST_WARNING_OBJECT(aamp, "Discard audio track- not sending data\n");
			return;
		}
#endif

		const char* mediaTypeStr = (mediaType == eMEDIATYPE_AUDIO) ? "eMEDIATYPE_AUDIO" : "eMEDIATYPE_VIDEO";
		GST_INFO_OBJECT(aamp, "Enter len = %d fpts %f mediaType %s", (int) pBuffer->len, fpts, mediaTypeStr);
		if (!readyToSend)
		{
			if (!gst_aamp_ready(aamp))
			{
				GST_WARNING_OBJECT(aamp, "Not ready to consume data type %s\n", mediaTypeStr);
				return;
			}
			readyToSend = true;
		}
		media_stream* stream = &aamp->stream[mediaType];
		if (!stream->srcpad)
		{
			GST_WARNING_OBJECT(aamp, "Pad NULL mediaType: %s (%d)  len = %d fpts %f\n", mediaTypeStr, mediaType,
			        (int) pBuffer->len, fpts);
			return;
		}

		GstClockTime pts = (GstClockTime)(fpts * GST_SECOND);
		GstClockTime dts = (GstClockTime)(fdts * GST_SECOND);
//#define TRACE_PTS_TRACK 0xff
#ifdef TRACE_PTS_TRACK
		if (( mediaType == TRACE_PTS_TRACK ) || (0xFF == TRACE_PTS_TRACK))
		{
			printf("%s : fpts %f pts %llu", (mediaType == eMEDIATYPE_VIDEO)?"vid":"aud", fpts, (unsigned long long)pts);
			GstClock *clock = GST_ELEMENT_CLOCK(aamp);
			if (clock)
			{
				GstClockTime curr = gst_clock_get_time(clock);
				printf(" provided clock time %lu diff (pts - curr) %lu (%lu ms)", (unsigned long)curr, (unsigned long)(pts-curr), (unsigned long)(pts-curr)/GST_MSECOND);
			}
			printf("\n");
		}
#endif

		if (stream->eventsPending)
		{
			SendPendingEvents(stream, pts);
			discontinuity = TRUE;
		}
#ifdef GSTAAMP_DUMP_STREAM
		static FILE* fp[2] =
		{	NULL,NULL};
		static char filename[128];
		if (!fp[mediaType])
		{
			sprintf(filename, "gstaampdump%d.ts",mediaType );
			fp[mediaType] = fopen(filename, "w");
		}
		fwrite(buffer->ptr, 1, buffer->len, fp[mediaType] );
#endif

		if (aamp->player_aamp->aamp->DownloadsAreEnabled())
		{
#ifdef USE_GST1
			GstBuffer* buffer = gst_buffer_new_wrapped (pBuffer->ptr ,pBuffer->len);
			GST_BUFFER_PTS(buffer) = pts;
			GST_BUFFER_DTS(buffer) = dts;
#else
			GstBuffer* buffer = gst_buffer_new();
			GST_BUFFER_SIZE(buffer) = pBuffer->len;
			GST_BUFFER_MALLOCDATA(buffer) = pBuffer->ptr;
			GST_BUFFER_DATA (buffer) = GST_BUFFER_MALLOCDATA(buffer)
			GST_BUFFER_TIMESTAMP(buffer) = pts;
			GST_BUFFER_DURATION(buffer) = duration;
#endif
			if (discontinuity)
			{
				GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
				discontinuity = FALSE;
			}
			gst_aamp_stream_add_item( stream, buffer);
		}
		/*Since ownership of buffer is given to gstreamer, reset pBuffer */
		memset(pBuffer, 0x00, sizeof(GrowableBuffer));
		GST_TRACE_OBJECT(aamp, "Exit");
	}


	/**
	 * @brief Updates internal rate
	 * @param[in] rate Rate at which media is played back
	 */
	void UpdateRate(gdouble rate)
	{
		if ( rate != this->rate)
		{
			this->rate = rate;
		}
	}

	/**
	 * @brief Handles stream specific EOS notification from aamp core
	 * @param[in] type Media type of the stream
	 */
	void EndOfStreamReached(MediaType type)
	{
		GST_WARNING_OBJECT(aamp, "MediaType %d", (int)type);
		media_stream* stream = &aamp->stream[type];
		if (stream->srcpad)
		{
			gst_aamp_stream_add_item( stream, gst_event_new_eos());
		}
	}


	/**
	 * @brief Handles Discontinuity of stream
	 * @param[in] mediaType type of stream
	 * @retval always false
	 */
	bool Discontinuity(MediaType mediaType)
	{
		aamp->stream[mediaType].resetPosition = TRUE;
		aamp->stream[mediaType].eventsPending = TRUE;
		return false;
	}

	/**
	 * @brief Flush gstaamp streams
	 * @param[in] position seek position
	 * @param[in] rate playback rate
	 */
	void Flush(double position, float rate)
	{
		for (int i = 0; i < AAMP_TRACK_COUNT; i++)
		{
			aamp->stream[i].resetPosition = TRUE;
			aamp->stream[i].flush = TRUE;
			aamp->stream[i].eventsPending = TRUE;
		}
	}
	unsigned long getCCDecoderHandle(void)
	{
		GstStructure *structure;
		GstQuery *query;
		const GValue *val;
		gboolean ret;
		gpointer decoder_handle = NULL;

		if (!aamp->stream[eMEDIATYPE_VIDEO].srcpad)
		{
			GST_DEBUG_OBJECT(aamp, "No video src pad available for AAMP plugin to query decoder handle\n");
		}
		else
		{
			structure = gst_structure_new("get_video_handle", "video_handle", G_TYPE_POINTER, 0, NULL);
#ifdef USE_GST1
			query = gst_query_new_custom(GST_QUERY_CUSTOM, structure);
#else
			query = gst_query_new_application(GST_QUERY_CUSTOM, structure);
#endif
			ret = gst_pad_peer_query(aamp->stream[eMEDIATYPE_VIDEO].srcpad, query);
			if (ret)
			{
				GST_DEBUG_OBJECT(aamp, "Video decoder handle queried successfully\n");
				structure = (GstStructure *) gst_query_get_structure(query);
				val = gst_structure_get_value(structure, "video_handle");

				if (val == NULL)
				{
					GST_ERROR_OBJECT(aamp, "Unable to retrieve video decoder handle from structure\n");
				}
				else
				{
					decoder_handle = g_value_get_pointer(val);
					GST_DEBUG_OBJECT(aamp, "video decoder handle: %x\n", decoder_handle);
				}
			}
			else
			{
				GST_ERROR_OBJECT(aamp, "Video decoder handle query failed \n");
			}

			gst_query_unref(query);
		}
		return (unsigned long)decoder_handle;
	}

	void Event(const AAMPEvent& event);
private:
	GstAamp * aamp;
	gdouble rate;
	bool srcPadCapsSent;
	StreamOutputFormat format;
	StreamOutputFormat audioFormat;
	bool readyToSend;
};

#define AAMP_TYPE_INIT_CODE { \
	GST_DEBUG_CATEGORY_INIT (gst_aamp_debug_category, "aamp", 0, \
		"debug category for aamp element"); \
	}
static GstStaticPadTemplate gst_aamp_sink_template_hls = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
		GST_STATIC_CAPS("application/x-hls;"
						"application/x-aamp;"));
static GstStaticPadTemplate gst_aamp_sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
		GST_STATIC_CAPS("application/x-aamp;"));

#define AAMP_SRC_CAPS_STR "video/mpegts, " \
        "  systemstream=(boolean)true, "\
        "  packetsize=(int)188;"
#define AAMP_SRC_AUDIO_CAPS_STR \
    "audio/mpeg, " \
      "mpegversion = (int) 1;" \
    "audio/mpeg, " \
      "mpegversion = (int) 2, " \
      "stream-format = (string) adts; " \
    "audio/x-ac3; audio/x-eac3;"

static GstStaticPadTemplate gst_aamp_src_template_video =
    GST_STATIC_PAD_TEMPLATE ("video_%02x",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
		GST_STATIC_CAPS(AAMP_SRC_CAPS_STR));

static GstStaticPadTemplate gst_aamp_src_template_audio =
    GST_STATIC_PAD_TEMPLATE ("audio_%02x",
        GST_PAD_SRC,
        GST_PAD_SOMETIMES,
		GST_STATIC_CAPS(AAMP_SRC_AUDIO_CAPS_STR));

/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstAamp, gst_aamp, GST_TYPE_ELEMENT, AAMP_TYPE_INIT_CODE);


/**
 * @brief Idle task to report decoder handle
 * @param[in] user_data  gstaamp pointer
 * @retval G_SOURCE_REMOVE if handle received, G_SOURCE_CONTINUE otherwise
 */
static gboolean gst_report_video_decode_handle(gpointer user_data)
{
	GstAamp *aamp = (GstAamp *) user_data;
	aamp->player_aamp->aamp->NotifyFirstFrameReceived();
	aamp->decoder_idle_id = 0;
	return G_SOURCE_REMOVE;
}


/**
 * @brief Updates audio src pad states.
 * @param[in] aamp gstaamp pointer
 */
static void gst_aamp_update_audio_src_pad(GstAamp * aamp)
{
#ifndef AAMP_DISCARD_AUDIO_TRACK
	if (NULL != aamp->stream[eMEDIATYPE_AUDIO].srcpad)
	{
		gboolean enable_audio;
		if ( aamp->rate != AAMP_NORMAL_PLAY_RATE)
		{
			enable_audio = FALSE;
		}
		else
		{
			enable_audio = TRUE;
		}
		if (enable_audio && !aamp->audio_enabled)
		{
			GST_INFO_OBJECT(aamp, "Enable aud and add pad");
			if (FALSE == gst_pad_set_active (aamp->stream[eMEDIATYPE_AUDIO].srcpad, TRUE))
			{
				GST_WARNING_OBJECT(aamp, "gst_pad_set_active failed");
			}
			if (FALSE == gst_element_add_pad(GST_ELEMENT(aamp), aamp->stream[eMEDIATYPE_AUDIO].srcpad))
			{
				GST_WARNING_OBJECT(aamp, "gst_element_add_pad stream[eMEDIATYPE_AUDIO].srcpad failed");
			}
			if (aamp->enable_src_tasks)
			{
				gst_aamp_stream_start(&aamp->stream[eMEDIATYPE_AUDIO]);
			}
			aamp->stream[eMEDIATYPE_AUDIO].streamStart = TRUE;
			aamp->stream[eMEDIATYPE_AUDIO].eventsPending = TRUE;
			aamp->audio_enabled = TRUE;
		}
		else if (!enable_audio && aamp->audio_enabled)
		{
			GST_INFO_OBJECT(aamp, "Disable aud and remove pad");
			if (FALSE == gst_pad_set_active (aamp->stream[eMEDIATYPE_AUDIO].srcpad, FALSE))
			{
				GST_WARNING_OBJECT(aamp, "gst_pad_set_active FALSE failed");
			}
			if (FALSE == gst_element_remove_pad(GST_ELEMENT(aamp), aamp->stream[eMEDIATYPE_AUDIO].srcpad))
			{
				GST_WARNING_OBJECT(aamp, "gst_element_remove_pad stream[eMEDIATYPE_AUDIO].srcpad failed");
			}
			aamp->audio_enabled = FALSE;
		}
	}
#endif
}


/**
 * @brief Creates GstCaps corresponding to stream format
 * @param[in] format Output format of stream
 * @retval GstCaps corresponding to stream format
 * @note Caller shall free returned caps
 */
static GstCaps* GetGstCaps(StreamOutputFormat format)
{
	GstCaps * caps = NULL;
	switch (format)
	{
		case FORMAT_MPEGTS:
			caps = gst_caps_new_simple ("video/mpegts",
					"systemstream", G_TYPE_BOOLEAN, TRUE,
					"packetsize", G_TYPE_INT, 188, NULL);
			break;
		case FORMAT_ISO_BMFF:
			caps = gst_caps_new_simple("video/quicktime", NULL, NULL);
			break;
		case FORMAT_AUDIO_ES_AAC:
			caps = gst_caps_new_simple ("audio/mpeg",
					"mpegversion", G_TYPE_INT, 2,
					"stream-format", G_TYPE_STRING, "adts", NULL);
			break;
		case FORMAT_AUDIO_ES_AC3:
			caps = gst_caps_new_simple ("audio/ac3", NULL, NULL);
			break;
		case FORMAT_AUDIO_ES_EC3:
			caps = gst_caps_new_simple ("audio/x-eac3", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_H264:
			caps = gst_caps_new_simple ("video/x-h264", NULL, NULL);
			break;
		case FORMAT_VIDEO_ES_MPEG2:
			caps = gst_caps_new_simple ("video/mpeg",
					"mpegversion", G_TYPE_INT, 2,
					"systemstream", G_TYPE_BOOLEAN, FALSE, NULL);
			break;
		case FORMAT_INVALID:
		case FORMAT_NONE:
		default:
			g_warning("Unsupported format %d\n", format);
			break;
	}
	return caps;
}

/**
 * @brief Initialize a stream.
 * @param[in] parent pointer to gstaamp instance
 * @param[out] stream pointer to stream structure
 */
void gst_aamp_initialize_stream( GstAamp* parent, media_stream* stream)
{
	stream->queue = g_queue_new ();
	stream->parent = parent;
	g_mutex_init (&stream->mutex);
	g_cond_init (&stream->cond);
}

/**
 * @brief Configures gstaamp with stream output formats
 * @param[in] aamp gstaamp pointer
 * @param[in] format Output format of main media
 * @param[in] audioFormat Output format of audio if present
 */
static void gst_aamp_configure(GstAamp * aamp, StreamOutputFormat format, StreamOutputFormat audioFormat)
{
	GstCaps *caps;
	gchar * padname = NULL;
	GST_DEBUG_OBJECT(aamp, "format %d audioFormat %d", format, audioFormat);

	g_mutex_lock (&aamp->mutex);
	if ( aamp->state >= GST_AAMP_CONFIGURED )
	{
		gst_aamp_update_audio_src_pad(aamp);
		g_mutex_unlock (&aamp->mutex);
		GST_INFO_OBJECT(aamp, "Already configured");
		return;
	}
	g_mutex_unlock (&aamp->mutex);
	if (aamp->player_aamp->aamp->IsMuxedStream())
	{
		GST_INFO_OBJECT(aamp, "Muxed stream, enable src pad tasks");
		aamp->enable_src_tasks = TRUE;
	}
	else
	{
		GST_INFO_OBJECT(aamp, "de-muxed stream, do not enable src pad tasks");
		aamp->enable_src_tasks = FALSE;
	}

	caps = GetGstCaps(format);

	if (caps)
	{
		media_stream* video = &aamp->stream[eMEDIATYPE_VIDEO];
		padname = g_strdup_printf ("video_%02x", 1);
		GstPad *srcpad = gst_pad_new_from_static_template(&gst_aamp_src_template_video, padname);
		gst_object_ref(srcpad);
		gst_pad_use_fixed_caps(srcpad);
		GST_OBJECT_FLAG_SET(srcpad, GST_PAD_FLAG_NEED_PARENT);
		gst_pad_set_query_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_query));
		gst_pad_set_event_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_event));
		video->caps = caps;
		video->srcpad = srcpad;
		gst_aamp_initialize_stream(aamp, video);
		if (padname)
		{
			GST_INFO_OBJECT(aamp, "Created pad %s", padname);
		    g_free (padname);
		}
		aamp->stream_id = gst_pad_create_stream_id(srcpad, GST_ELEMENT(aamp), NULL);
	}
	else
	{
		GST_WARNING_OBJECT(aamp, "Unsupported videoFormat %d", format);
	}

	caps = GetGstCaps(audioFormat);
	if (caps)
	{
		media_stream* audio = &aamp->stream[eMEDIATYPE_AUDIO];
		padname = g_strdup_printf ("audio_%02x", 1);
		GstPad *srcpad = gst_pad_new_from_static_template(&gst_aamp_src_template_audio, padname);
		g_free(padname);
		gst_object_ref(srcpad);
		gst_pad_use_fixed_caps(srcpad);
		GST_OBJECT_FLAG_SET(srcpad, GST_PAD_FLAG_NEED_PARENT);
		gst_pad_set_query_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_query));
		gst_pad_set_event_function(srcpad, GST_DEBUG_FUNCPTR(gst_aamp_src_event));
		audio->srcpad = srcpad;
		audio->caps= caps;
		gst_aamp_initialize_stream(aamp, audio);
	}
	else
	{
		GST_WARNING_OBJECT(aamp, "Unsupported audioFormat %d", audioFormat);
	}

	g_mutex_lock (&aamp->mutex);
	aamp->state = GST_AAMP_CONFIGURED;
	g_cond_signal(&aamp->state_changed);
	g_mutex_unlock (&aamp->mutex);
}


/**
 * @brief Invoked by gstreamer core to initialize class.
 * @param[in] klass GstAampClass pointer
 */
static void gst_aamp_class_init(GstAampClass * klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

	g_aamp_expose_hls_caps = g_getenv ("GST_AAMP_EXPOSE_HLS_CAPS");
	if (g_aamp_expose_hls_caps)
	{
		gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_sink_template_hls));
	}
	else
	{
		gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_sink_template));
	}
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_src_template_audio));
	gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_aamp_src_template_video));

	gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "Advanced Adaptive Media Player", "Demux",
			"Advanced Adaptive Media Player", "Comcast");

	gobject_class->finalize = gst_aamp_finalize;
	element_class->change_state = GST_DEBUG_FUNCPTR(gst_aamp_change_state);
	element_class->query = GST_DEBUG_FUNCPTR(gst_aamp_query);
}


/**
 * @brief Invoked by gstreamer core to initialize element.
 * @param[in] aamp gstaamp pointer
 */
static void gst_aamp_init(GstAamp * aamp)
{
	GST_AAMP_LOG_TIMING("Enter\n");
	aamp->location = NULL;
	aamp->rate = AAMP_NORMAL_PLAY_RATE;
	aamp->audio_enabled = FALSE;
	aamp->state = GST_AAMP_NONE;
	aamp->context = new GstAampStreamer(aamp);
	aamp->player_aamp = new PlayerInstanceAAMP(aamp->context);
	aamp->sinkpad = gst_pad_new_from_static_template(&gst_aamp_sink_template_hls, "sink");
	memset(&aamp->stream[0], 0 , sizeof(aamp->stream));
	aamp->stream_id = NULL;
	aamp->idle_id = 0;
	aamp->enable_src_tasks = FALSE;
	aamp->decoder_idle_id = 0;

	gst_pad_set_chain_function(aamp->sinkpad, GST_DEBUG_FUNCPTR(gst_aamp_sink_chain));
	gst_pad_set_event_function(aamp->sinkpad, GST_DEBUG_FUNCPTR(gst_aamp_sink_event));
	gst_element_add_pad(GST_ELEMENT(aamp), aamp->sinkpad);
	g_mutex_init (&aamp->mutex);
	g_cond_init (&aamp->state_changed);
	aamp->context->Discontinuity(eMEDIATYPE_VIDEO);
	aamp->context->Discontinuity(eMEDIATYPE_AUDIO);
}

/**
 * @brief Finalize a stream.
 * @param[in] stream pointer to stream structure
 */
void gst_aamp_finalize_stream( media_stream* stream)
{
	if (stream->srcpad)
	{
		if (stream->caps)
		{
			gst_caps_unref(stream->caps);
		}

		if (stream->srcpad)
		{
			gst_object_unref(stream->srcpad);
		}

		if (stream->queue)
		{
			g_queue_free(stream->queue);
		}
		g_mutex_clear(&stream->mutex);
		g_cond_clear(&stream->cond);
	}
}

/**
 * @brief Invoked by gstreamer core to finalize element.
 * @param[in] object gstaamp pointer
 */
void gst_aamp_finalize(GObject * object)
{
	GstAamp *aamp = GST_AAMP(object);

	if (aamp->location)
	{
		g_free(aamp->location);
		aamp->location = NULL;
	}
	g_mutex_clear (&aamp->mutex);
	delete aamp->player_aamp;
	delete aamp->context;
	aamp->context=NULL;
	g_cond_clear (&aamp->state_changed);

	gst_aamp_finalize_stream( &aamp->stream[eMEDIATYPE_VIDEO]);
	gst_aamp_finalize_stream(&aamp->stream[eMEDIATYPE_AUDIO]);

	if (aamp->stream_id)
	{
		g_free(aamp->stream_id);
	}

	GST_AAMP_LOG_TIMING("Exit\n");
	G_OBJECT_CLASS(gst_aamp_parent_class)->finalize(object);
}

/**
 * @brief This function processes asynchronous events from aamp core
 * @param[in] e reference of event
 */
void GstAampStreamer::Event(const AAMPEvent & e )
{
		switch (e.type)
		{
			case AAMP_EVENT_TUNED:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_TUNED");
				break;

			case AAMP_EVENT_TUNE_FAILED:
				GST_WARNING_OBJECT(aamp, "Tune failed");
				g_mutex_lock (&aamp->mutex);
				aamp->state = GST_AAMP_STATE_ERROR;
				g_cond_signal(&aamp->state_changed);
				g_mutex_unlock (&aamp->mutex);
				break;

			case AAMP_EVENT_EOS:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_EOS");
				break;

			case AAMP_EVENT_PLAYLIST_INDEXED:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_PLAYLIST_INDEXED");
				break;

			case AAMP_EVENT_PROGRESS:
				break;

			case AAMP_EVENT_TIMED_METADATA:
				GST_INFO_OBJECT(aamp, "AAMP_EVENT_TIMED_METADATA");
				break;

			default:
				GST_DEBUG_OBJECT(aamp, "unknown event %d\n", e.type);
				break;
		}
}


/**
 * @brief Queries URI to upstream element
 * @param[in] aamp gstaamp pointer
 * @retval
 */
static gboolean gst_aamp_query_uri(GstAamp *aamp)
{
	gboolean ret = TRUE;
	GstQuery *query = gst_query_new_uri();

	ret = gst_pad_peer_query(aamp->sinkpad, query);
	if (ret)
	{
		gchar *uri;
		gst_query_parse_uri(query, &uri);
		GST_DEBUG_OBJECT(aamp, "uri %s\n", uri);
		if (aamp->location)
		{
			g_free(aamp->location);
		}
		aamp->location = g_strdup(uri);
		g_free(uri);
	}
	gst_query_unref(query);
	return ret;
}


/**
 * @brief Tune asynchronously
 * @param[in] aamp gstaamp pointer
 */
static void gst_aamp_tune_async(GstAamp *aamp)
{
	g_mutex_lock(&aamp->mutex);
	aamp->state = GST_AAMP_TUNING;
	g_mutex_unlock(&aamp->mutex);
	GST_AAMP_LOG_TIMING("Calling aamp->Tune()\n");
	aamp->player_aamp->Tune(aamp->location);
}


/**
 * @brief Waits until aamp is configured
 * @param[in] aamp gstaamp pointer
 * @retval TRUE if configured, FALSE on error
 */
static gboolean gst_aamp_configured(GstAamp *aamp)
{
	gboolean ret = FALSE;
	g_mutex_lock(&aamp->mutex);
	if ( aamp->state == GST_AAMP_TUNING )
	{
		g_cond_wait(&aamp->state_changed, &aamp->mutex);
	}
	ret = (aamp->state >= GST_AAMP_CONFIGURED);
	g_mutex_unlock(&aamp->mutex);
	return ret;
}


/**
 * @brief Waits until gstaamp state is ready
 * @param[in] aamp gstaamp pointer
 * @retval TRUE if ready, FALSE on error
 */
static gboolean gst_aamp_ready(GstAamp *aamp)
{
	gboolean ret = FALSE;
	g_mutex_lock(&aamp->mutex);
	while (aamp->state < GST_AAMP_READY)
	{
		g_cond_wait(&aamp->state_changed, &aamp->mutex);
	}
	ret = (aamp->state == GST_AAMP_READY);
	g_mutex_unlock(&aamp->mutex);
	return ret;
}


/**
 * @brief Idle task to report tune complete
 * @param[in] user_data  gstaamp pointer
 * @retval G_SOURCE_REMOVE if logging done, G_SOURCE_CONTINUE if state is not playing
 */
static gboolean gst_aamp_report_on_tune_done(gpointer user_data)
{
	GstAamp *aamp = (GstAamp *) user_data;
	GstElement *pbin = GST_ELEMENT(aamp);
	while (GST_ELEMENT_PARENT(pbin))
	{
		pbin = GST_ELEMENT_PARENT(pbin);
	}
	if (GST_STATE(pbin) == GST_STATE_PLAYING)
	{
		GST_AAMP_LOG_TIMING("LogTuneComplete()");
		aamp->player_aamp->aamp->LogTuneComplete();
		aamp->idle_id = 0;
		return G_SOURCE_REMOVE;
	}
	else
	{
		GST_DEBUG_OBJECT(aamp, "Pipeline Not yet playing");
		return G_SOURCE_CONTINUE;
	}
}


/**
 * @brief Invoked by gstreamer core to change element state.
 * @param[in] element gstaamp pointer
 * @param[in] trans state
 * @retval status of state change operation
 */
static GstStateChangeReturn gst_aamp_change_state(GstElement * element, GstStateChange trans)
{
	GstAamp *aamp;
	GstStateChangeReturn ret;

	aamp = GST_AAMP(element);
	GST_DEBUG_OBJECT(aamp, "Enter");

	switch (trans)
	{
		case GST_STATE_CHANGE_NULL_TO_READY:
			GST_AAMP_LOG_TIMING("GST_STATE_CHANGE_NULL_TO_READY");
			aamp->player_aamp->RegisterEvents(aamp->context);
			if ( FALSE == gst_aamp_query_uri( aamp) )
			{
				return GST_STATE_CHANGE_FAILURE;
			}
#ifdef AAMP_JSCONTROLLER_ENABLED
			{
				int sessionId = 0;
				if (aamp->location)
				{
					char *sessionValue = strstr(aamp->location, "?sessionId=");
					if (sessionValue != NULL)
					{
						sscanf(sessionValue + 1, "sessionId=%d", &sessionId);
					}
				}
				setAAMPPlayerInstance(aamp->player_aamp, sessionId);
			}
#endif
			gst_aamp_tune_async( aamp);
			aamp->report_tune = TRUE;
			aamp->report_decode_handle = TRUE;
			aamp->player_aamp->aamp->ResumeTrackDownloads(eMEDIATYPE_VIDEO);
			aamp->player_aamp->aamp->ResumeTrackDownloads(eMEDIATYPE_AUDIO);
			break;

		case GST_STATE_CHANGE_READY_TO_PAUSED:
			GST_AAMP_LOG_TIMING("GST_STATE_CHANGE_READY_TO_PAUSED\n");
			g_mutex_lock (&aamp->mutex);
			if (NULL != aamp->stream[eMEDIATYPE_VIDEO].srcpad)
			{
				if (FALSE == gst_pad_set_active (aamp->stream[eMEDIATYPE_VIDEO].srcpad, TRUE))
				{
					GST_WARNING_OBJECT(aamp, "gst_pad_set_active failed");
				}
				if (FALSE == gst_element_add_pad(GST_ELEMENT(aamp), aamp->stream[eMEDIATYPE_VIDEO].srcpad))
				{
					GST_WARNING_OBJECT(aamp, "gst_element_add_pad srcpad failed");
				}
				if (aamp->enable_src_tasks)
				{
					gst_aamp_stream_start(&aamp->stream[eMEDIATYPE_VIDEO]);
				}
				aamp->stream[eMEDIATYPE_VIDEO].streamStart = TRUE;
				aamp->stream[eMEDIATYPE_VIDEO].eventsPending = TRUE;
			}
			gst_aamp_update_audio_src_pad(aamp);
			aamp->state = GST_AAMP_READY;
			g_cond_signal(&aamp->state_changed);
			g_mutex_unlock (&aamp->mutex);
			gst_element_no_more_pads (element);
			break;
		case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
			GST_AAMP_LOG_TIMING("GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
//Invoke idle handler
			if (aamp->report_tune)
			{
				aamp->idle_id = g_timeout_add(50, gst_aamp_report_on_tune_done, aamp);
				aamp->report_tune = FALSE;
			}

			if (aamp->report_decode_handle)
			{
				aamp->decoder_idle_id = g_idle_add(gst_report_video_decode_handle, aamp);
				aamp->report_decode_handle = FALSE;
			}
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			if (aamp->enable_src_tasks)
			{
				gst_aamp_stop_and_flush(aamp);
			}
			break;

		default:
			break;
	}

	ret = GST_ELEMENT_CLASS(gst_aamp_parent_class)->change_state(element, trans);
	if (ret == GST_STATE_CHANGE_FAILURE)
	{
		GST_ERROR_OBJECT(aamp, "Parent state change failed\n");
		return ret;
	}
	else
	{
		GST_DEBUG_OBJECT(aamp, "Parent state change :  %s\n", gst_element_state_change_return_get_name(ret));
	}

	switch (trans)
	{
		case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
			if (aamp->idle_id)
			{
				g_source_remove(aamp->idle_id);
				aamp->idle_id = 0;
			}
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_PLAYING_TO_PAUSED");
			break;
		case GST_STATE_CHANGE_PAUSED_TO_READY:
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_PAUSED_TO_READY");
			if (aamp->decoder_idle_id)
			{
				g_source_remove(aamp->decoder_idle_id);
				aamp->decoder_idle_id = 0;
			}
			g_mutex_lock(&aamp->mutex);
			aamp->state = GST_AAMP_SHUTTING_DOWN;
			g_cond_signal(&aamp->state_changed);
			g_mutex_unlock(&aamp->mutex);
			aamp->player_aamp->Stop();
			break;
		case GST_STATE_CHANGE_READY_TO_NULL:
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_READY_TO_NULL");
			aamp->player_aamp->RegisterEvents(NULL);
#ifdef AAMP_JSCONTROLLER_ENABLED
			unsetAAMPPlayerInstance(aamp->player_aamp);
#endif
			break;
		case GST_STATE_CHANGE_NULL_TO_READY:
			if (!gst_aamp_configured(aamp))
			{
				GST_ERROR_OBJECT(aamp, "Not configured");
				return GST_STATE_CHANGE_FAILURE;
			}
			else
			{
				GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_NULL_TO_READY Complete");
			}
			break;
		case GST_STATE_CHANGE_READY_TO_PAUSED:
			if (aamp->player_aamp->aamp->IsLive())
			{
				GST_INFO_OBJECT(aamp, "LIVE stream");
				ret = GST_STATE_CHANGE_NO_PREROLL;
			}
			GST_DEBUG_OBJECT(aamp, "GST_STATE_CHANGE_READY_TO_PAUSED");
			break;
		default:
			break;
	}
	GST_DEBUG_OBJECT(aamp, "Exit");

	return ret;
}


/**
 * @brief Element query override .invoked by gstreamer core
 * @param[in] element gstaamp pointer
 * @param[in] query gstreamer query
 * @retval TRUE if query is handled, FALSE if not handled
 */
static gboolean gst_aamp_query(GstElement * element, GstQuery * query)
{
	GstAamp *aamp = GST_AAMP(element);
	gboolean ret = FALSE;

	GST_INFO_OBJECT(aamp, " query %s\n", gst_query_type_get_name(GST_QUERY_TYPE(query)));

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_POSITION:
		{
			GstFormat format;

			gst_query_parse_position(query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gst_query_set_position(query, GST_FORMAT_TIME, (aamp->player_aamp->aamp->GetPositionMs()*GST_MSECOND));
				ret = TRUE;
			}
			break;
		}

		case GST_QUERY_DURATION:
		{
			GstFormat format;
			gst_query_parse_duration (query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gint64 duration = aamp->player_aamp->aamp->GetDurationMs()*GST_MSECOND;
				gst_query_set_duration (query, format, duration);
				GST_TRACE_OBJECT(aamp, " GST_QUERY_DURATION returning duration %"G_GUINT64_FORMAT"\n", duration);
				ret = TRUE;
			}
			else
			{
				const GstFormatDefinition* def =  gst_format_get_details(format);
				GST_WARNING_OBJECT(aamp, " GST_QUERY_DURATION format %s %s\n", def->nick, def->description);
			}
			break;
		}

		case GST_QUERY_SCHEDULING:
		{
			gst_query_set_scheduling (query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
			ret = TRUE;
			break;
		}

		default:
			break;
	}

	if (FALSE == ret )
	{
		ret = GST_ELEMENT_CLASS(gst_aamp_parent_class)->query(element, query);
	}
	else
	{
		gst_query_unref(query);
	}
	return ret;
}


/**
 * @brief Chain function to handle buffer from source elements
 * @param[in] pad sink pad
 * @param[in] parent gstaamp pointer
 * @param[in] buffer Data from upstream
 * @retval status of operation
 */
static GstFlowReturn gst_aamp_sink_chain(GstPad * pad, GstObject *parent, GstBuffer * buffer)
{
	GstAamp *aamp;

	aamp = GST_AAMP(parent);
	GST_DEBUG_OBJECT(aamp, "chain");
	gst_buffer_unref(buffer);
	return GST_FLOW_OK;
}

/**
 * @brief Event listener on sink pad to be invoked by gstreamer core
 * @param[in] pad sink pad
 * @param[in] parent gstaamp pointer
 * @param[in] event gstreamer event
 * @retval TRUE if event is handled, FALSE if not handled
 */
static gboolean gst_aamp_sink_event(GstPad * pad, GstObject *parent, GstEvent * event)
{
	gboolean res = FALSE;
	GstAamp *aamp = GST_AAMP(parent);

	GST_INFO_OBJECT(aamp, " EVENT %s\n", gst_event_type_get_name(GST_EVENT_TYPE(event)));

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_EOS:
			GST_WARNING_OBJECT(aamp, "sink pad : Got EOS\n" );
			gst_event_unref(event);
			res = TRUE;
			break;
#ifdef AAMP_SEND_SEGMENT_EVENTS
		case GST_EVENT_SEGMENT:
			GST_INFO_OBJECT(aamp, "sink pad : Got Segment\n" );
			gst_event_unref(event);
			res = TRUE;
			break;
#endif
		default:
			res = gst_pad_event_default(pad, parent, event);
			break;
	}
	return res;
}

/**
 * @brief src pad query listener override to be invoked by gstreamer core
 * @param[in] pad src pad
 * @param[in] parent gstaamp pointer
 * @param[in] query gstreamer query
 * @retval TRUE if query is handled, FALSE if not query
 */
static gboolean gst_aamp_src_query(GstPad * pad, GstObject *parent, GstQuery * query)
{
	gboolean ret = FALSE;
	GstAamp *aamp = GST_AAMP(parent);

	GST_TRACE_OBJECT(aamp, " query %s\n", gst_query_type_get_name(GST_QUERY_TYPE(query)));

	switch (GST_QUERY_TYPE(query))
	{
		case GST_QUERY_CAPS:
		{
			GstCaps* caps;
			if(aamp->stream[eMEDIATYPE_VIDEO].srcpad == pad )
			{
				caps = aamp->stream[eMEDIATYPE_VIDEO].caps;
			}
			else if(aamp->stream[eMEDIATYPE_AUDIO].srcpad == pad )
			{
				caps = aamp->stream[eMEDIATYPE_AUDIO].caps;
			}
			else
			{
				GST_WARNING_OBJECT(aamp, "Unknown pad %p", pad);
			}
			gst_query_set_caps_result(query, caps);
			ret = TRUE;
			break;
		}
		case GST_QUERY_POSITION:
		{
			GstFormat format;

			gst_query_parse_position(query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gint64 posMs = aamp->player_aamp->aamp->GetPositionMs();
				GST_TRACE_OBJECT(aamp, " GST_QUERY_POSITION position %"G_GUINT64_FORMAT" seconds\n", posMs/1000);
				gst_query_set_position(query, GST_FORMAT_TIME, (posMs*GST_MSECOND ));
				ret = TRUE;
			}
			break;
		}

		case GST_QUERY_DURATION:
		{
			GstFormat format;
			gst_query_parse_duration (query, &format, NULL);
			if (format == GST_FORMAT_TIME)
			{
				gint64 duration = aamp->player_aamp->aamp->GetDurationMs()*GST_MSECOND;
				gst_query_set_duration (query, format, duration);
				GST_TRACE_OBJECT(aamp, " GST_QUERY_DURATION returning duration %"G_GUINT64_FORMAT"\n", duration);
				ret = TRUE;
			}
			else
			{
				const GstFormatDefinition* def =  gst_format_get_details(format);
				GST_WARNING_OBJECT(aamp, " GST_QUERY_DURATION format %s %s\n", def->nick, def->description);
			}
			break;
		}
		case GST_QUERY_SCHEDULING:
		{
			gst_query_set_scheduling (query, GST_SCHEDULING_FLAG_SEEKABLE, 1, -1, 0);
			ret = TRUE;
			break;
		}

		case GST_QUERY_CUSTOM:
		{
		//g_print("\n\n\nReceived custom event\n\n\n");
			GstStructure *structure = gst_query_writable_structure(query);
			if (structure && gst_structure_has_name(structure, "get_aamp_instance"))
			{
				GValue val = { 0, };
				g_value_init(&val, G_TYPE_POINTER);
				g_value_set_pointer(&val, (gpointer) aamp->player_aamp->aamp);
				gst_structure_set_value(structure, "aamp_instance", &val);
				ret = TRUE;
			} else
			{
				ret = FALSE;
			}
			break;
		}

		default:
			break;
	}
	if (FALSE == ret )
	{
		GST_DEBUG_OBJECT(aamp, "Execute default handler for query %s\n", gst_query_type_get_name(GST_QUERY_TYPE(query)));
		ret = gst_pad_query_default(pad, parent, query);
	}
	return ret;
}


/**
 * @brief Event listener on src pad to be invoked by gstreamer core
 * @param[in] pad src pad
 * @param[in] parent gstaamp pointer
 * @param[in] event gstreamer event
 * @retval TRUE if event is handled, FALSE if not handled
 */
static gboolean gst_aamp_src_event(GstPad * pad, GstObject *parent, GstEvent * event)
{
	gboolean res = FALSE;
	GstAamp *aamp = GST_AAMP(parent);

	GST_DEBUG_OBJECT(aamp, " EVENT %s\n", gst_event_type_get_name(GST_EVENT_TYPE(event)));

	switch (GST_EVENT_TYPE(event))
	{
		case GST_EVENT_SEEK:
		{
			gdouble rate = 0;
			GstFormat format;
			GstSeekFlags flags;
			GstSeekType start_type;
			gint64 start = 0;
			GstSeekType stop_type;
			gint64 stop;
			gst_event_parse_seek(event, &rate, &format, &flags, &start_type, &start, &stop_type, &stop);
			if (format == GST_FORMAT_TIME)
			{
				GST_INFO_OBJECT(aamp, "sink pad : seek GST_FORMAT_TIME: rate %f, pos %"G_GINT64_FORMAT"\n", rate, start );
				if (flags & GST_SEEK_FLAG_FLUSH)
				{
					gst_aamp_stop_and_flush(aamp);
					if (aamp->enable_src_tasks)
					{
						gst_aamp_stream_start(&aamp->stream[eMEDIATYPE_VIDEO]);
						if (aamp->audio_enabled)
						{
							gst_aamp_stream_start(&aamp->stream[eMEDIATYPE_AUDIO]);
						}
					}
				}
				if (rate != aamp->rate)
				{
					aamp->context->UpdateRate(rate);
					aamp->rate  = rate;
				}

				if (start_type == GST_SEEK_TYPE_NONE)
				{
					aamp->player_aamp->SetRate(rate);
				}
				else if (start_type == GST_SEEK_TYPE_SET)
				{
					double pos;
					if (rate < 0)
					{
						pos = stop / GST_SECOND;
					}
					else
					{
						pos = start / GST_SECOND;
					}
					aamp->player_aamp->SetRateAndSeek(rate, pos);
				}
				else
				{
					GST_WARNING_OBJECT(aamp, "Not supported");
				}
				res = TRUE;
			}
			break;
		}

		default:
			break;
	}
	if (FALSE == res )
	{
		res = gst_pad_event_default(pad, parent, event);
	}
	else
	{
		gst_event_unref(event);
	}

	return res;
}
