/* gcc gdk-gstappsrc-stream.c -Wall `pkg-config --cflags --libs gstreamer-app-0.10 gdk-pixbuf-2.0` -o gdkstream */

#include <gst/gst.h>
// #include <gst/app/gstappsrc.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

GST_DEBUG_CATEGORY (appsrc_pipeline_debug);
#define GST_CAT_DEFAULT appsrc_pipeline_debug

typedef struct _App App;

struct _App
{
  GstElement *pipeline;
  GstElement *appsrc;
  GstElement *bin;

  GMainLoop *loop;
  guint sourceid;

  GTimer *timer;

};

App s_app;

static gboolean
read_data (App * app)
{
    GstFlowReturn ret;
    gdouble ms;

    ms = g_timer_elapsed(app->timer, NULL);
    if (ms > 1.0) {
        GstBuffer *buffer;
        GdkPixbuf *pb;
        gboolean ok = TRUE;

        buffer = gst_buffer_new_allocate(NULL, 4096, NULL);

        GstMapInfo info;
        gst_buffer_map(buffer, &info, GST_MAP_WRITE);
        unsigned char *buf = info.data;
        static int idx = 0;
        memset(buf, idx++, 1024);
        memset(buf + 1024, idx++, 1024);
        memset(buf + 2048, idx++, 1024);
        memset(buf + 3072, idx++, 1024);
        gst_buffer_unmap(buffer, &info);

        GST_DEBUG ("feed buffer");
        g_signal_emit_by_name (app->appsrc, "push-buffer", buffer, &ret);

        gst_buffer_unref (buffer);

        if (ret != GST_FLOW_OK) {
            /* some error, stop sending data */
            GST_DEBUG ("some error");
            ok = FALSE;
        }

        g_timer_start(app->timer);

        return ok;
    }

    //  g_signal_emit_by_name (app->appsrc, "end-of-stream", &ret);
    return TRUE;
}

/* This signal callback is called when appsrc needs data, we add an idle handler
 * to the mainloop to start pushing data into the appsrc */
static void
start_feed (GstElement * pipeline, guint size, App * app)
{
  if (app->sourceid == 0) {
    GST_DEBUG ("start feeding");
    app->sourceid = g_timeout_add (10, (GSourceFunc) read_data, app);
  }
}

/* This callback is called when appsrc has enough data and we can stop sending.
 * We remove the idle handler from the mainloop */
static void
stop_feed (GstElement * pipeline, App * app)
{
  if (app->sourceid != 0) {
    GST_DEBUG ("stop feeding");
    g_source_remove (app->sourceid);
    app->sourceid = 0;
  }
}

static gboolean
bus_message (GstBus * bus, GstMessage * message, App * app)
{
  GST_DEBUG ("got message %s",
      gst_message_type_get_name (GST_MESSAGE_TYPE (message)));

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg_info = NULL;

        gst_message_parse_error (message, &err, &dbg_info);
        g_printerr ("ERROR from element %s: %s\n",
            GST_OBJECT_NAME (message->src), err->message);
        g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
        g_error_free (err);
        g_free (dbg_info);
        g_main_loop_quit (app->loop);
        break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit (app->loop);
      break;
    default:
      break;
  }
  return TRUE;
}

int
main (int argc, char *argv[])
{
  App *app = &s_app;
  GstBus *bus;
  GstCaps *caps;

  gst_init (&argc, &argv);

  GST_DEBUG_CATEGORY_INIT (appsrc_pipeline_debug, "appsrc-pipeline", 0,
      "appsrc pipeline example");

  /* create a mainloop to get messages and to handle the idle handler that will
   * feed data to appsrc. */
  app->loop = g_main_loop_new (NULL, TRUE);
  app->timer = g_timer_new();

  app->pipeline = gst_pipeline_new("pipeline0");
  g_assert (app->pipeline);

  GError *video_error = NULL;
  char *video_desc =
    g_strdup_printf
    (
      "appsrc name=mysource max-bytes=90000000 ! videoparse width=1024 height=800 framerate=30/1 ! autovideosink"
    );
    // gst-launch-1.0 -v videotestsrc ! x264enc ! video/x-h264! rtph264pay pt=96 mtu=1200 ! udpsink host=127.0.0.1 port=6000

  app->bin = gst_parse_bin_from_description (video_desc, TRUE, &video_error);
  if (video_error) {
    printf ("video bin fail %s\n", video_error->message);
    return -1;
  }

  gst_bin_add_many(GST_BIN(app->pipeline), app->bin, NULL);
  printf("g bin add many returned\n");

  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  g_assert(bus);

  /* add watch for messages */
  gst_bus_add_watch (bus, (GstBusFunc) bus_message, app);

  /* get the appsrc */
    app->appsrc = gst_bin_get_by_name (GST_BIN(app->pipeline), "mysource");
    g_assert(app->appsrc);
    // g_assert(GST_IS_APP_SRC(app->appsrc));
    g_signal_connect (app->appsrc, "need-data", G_CALLBACK (start_feed), app);
    g_signal_connect (app->appsrc, "enough-data", G_CALLBACK (stop_feed), app);

  /* go to playing and wait in a mainloop. */
  gst_element_set_state (app->pipeline, GST_STATE_PLAYING);

  /* this mainloop is stopped when we receive an error or EOS */
  g_main_loop_run (app->loop);

  GST_DEBUG ("stopping");

  gst_element_set_state (app->pipeline, GST_STATE_NULL);

  gst_object_unref (bus);
  g_main_loop_unref (app->loop);

  return 0;
}