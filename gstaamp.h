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

#ifndef _GST_AAMP_H_
#define _GST_AAMP_H_

#include <gst/gst.h>
G_BEGIN_DECLS

#define GST_TYPE_AAMP   (gst_aamp_get_type())
#define GST_AAMP(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AAMP,GstAamp))
#define GST_AAMP_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AAMP,GstAampClass))
#define GST_IS_AAMP(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AAMP))
#define GST_IS_AAMP_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AAMP))

typedef struct _GstAamp GstAamp;
typedef struct _GstAampClass GstAampClass;

struct GstAampStreamer;
class PlayerInstanceAAMP;

enum _GstAampState {
	GST_AAMP_NONE,
	GST_AAMP_TUNING,
	GST_AAMP_CONFIGURED,
	GST_AAMP_READY,
	GST_AAMP_SHUTTING_DOWN,
	GST_AAMP_STATE_ERROR
};

typedef enum _GstAampState GstAampState;

struct media_stream
{
	GstPad *srcpad;
	gboolean flush;
	gboolean resetPosition;
	gboolean streamStart;
	gboolean eventsPending;
	GstCaps *caps;
};

struct _GstAamp
{
	GstElement parent_aamp;
	GstPad *sinkpad;
	media_stream stream[2];
	gboolean audio_enabled;
	gchar *location;
	float rate;
	GMutex mutex;
	GstAampStreamer* context;
	GCond state_changed;
	GstAampState state;
	gchar* stream_id;
	guint idle_id;
	gboolean report_tune;

#ifdef AAMP_CC_ENABLED
	GThread *cc_handler_id;
	gpointer video_decode_handle;
	gboolean quit_cc_handler;
#endif

	PlayerInstanceAAMP* player_aamp;
};

struct _GstAampClass
{
	GstElementClass base_aamp_class;
};

GType gst_aamp_get_type(void);

G_END_DECLS

#endif
