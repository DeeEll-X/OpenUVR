#include "gst_encode.h"
#include "ouvr_packet.h"
#include <gst/gst.h>
#include <pthread.h>

#define WIDTH 1920
#define HEIGHT 1080

static const int BUFF_SIZE = WIDTH * HEIGHT * 4;

typedef struct gst_encode_context
{
    pthread_t main_thread;
    GMainLoop * loop;
    GstElement *pipeline;
    GstElement * bin;
    GstElement * src;
    GstElement * sink;
    GstBus *bus;
} gst_encode_context;


gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
  GMainLoop *loop = (GMainLoop *)data;

  switch (GST_MESSAGE_TYPE(msg)) {

  case GST_MESSAGE_EOS:
    fprintf(stderr, "End of stream\n");
    g_main_loop_quit(loop);
    break;

  case GST_MESSAGE_ERROR: {
    gchar *debug;
    GError *error;

    gst_message_parse_error(msg, &error, &debug);
    g_free(debug);

    g_printerr("Error: %s\n", error->message);
    g_error_free(error);

    g_main_loop_quit(loop);

    break;
  }
  default:
    break;
  }

  return TRUE;
}

static GstFlowReturn need_data (GstElement *elem, guint length, struct ouvr_ctx *ctx) {
    // usleep(30000);
    GstFlowReturn ret;
    static int num_frame = 0;
    num_frame++;

    const uint8_t *const src = ctx->pix_buf;
    gst_encode_context *e = ctx->enc_priv;

    GstBuffer *buffer;
    buffer = gst_buffer_new_allocate(NULL, BUFF_SIZE, NULL);
    GST_BUFFER_TIMESTAMP(buffer) = (GstClockTime)((num_frame / 60.0) * 1e9);

    GstMapInfo info;
    gst_buffer_map(buffer, &info, GST_MAP_WRITE | GST_MAP_READ);
    unsigned char *buf = info.data;
    memmove(buf, src, BUFF_SIZE);
    gst_buffer_unmap(buffer, &info);

    g_signal_emit_by_name (e->src, "push-buffer", buffer, &ret);
#ifdef UE4DEBUG
    printf("push-buffer emited\n");
#endif

    gst_buffer_unref (buffer);

    if (ret != GST_FLOW_OK) {
        printf ("push-buffer fail\n");
        return -1;
    }
    return GST_FLOW_OK;
}


static void
bus_msg (GstBus * bus, GstMessage * msg, gpointer pipe)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:{
      gchar *error_file = g_strdup_printf ("error-%s", GST_OBJECT_NAME (pipe));

      GST_ERROR ("Error: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, error_file);
      g_free (error_file);
      printf ("Error received on bus");
      break;
    }
    case GST_MESSAGE_WARNING:{
      gchar *warn_file = g_strdup_printf ("warning-%s", GST_OBJECT_NAME (pipe));

      GST_WARNING ("Warning: %" GST_PTR_FORMAT, msg);
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipe),
          GST_DEBUG_GRAPH_SHOW_ALL, warn_file);
      g_free (warn_file);
      break;
    }
    case GST_MESSAGE_STATE_CHANGED:{
      /* We are only interested in state-changed messages from the pipeline */
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
        printf ("%s state changed from %s to %s, pending %s\n",
            GST_OBJECT_NAME (msg->src), gst_element_state_get_name (old_state), gst_element_state_get_name (new_state),
            gst_element_state_get_name (pending_state));
    }
    default:
      break;
  }
}

static int gst_initialize(struct ouvr_ctx *ctx)
{
    printf("enter gst initialize\n");
    GstStateChangeReturn ret;
    GError *video_error=NULL;
    GstState state;
    GstState pending;

    gst_init (NULL, NULL);

    if (ctx->enc_priv != NULL)
    {
        free(ctx->enc_priv);
    }
    gst_encode_context *e = calloc(1, sizeof(gst_encode_context));
    ctx->enc_priv = e;

    e->pipeline = gst_pipeline_new("pipeline0");

    char *video_desc =
      g_strdup_printf
      (
      // "videotestsrc "
      "appsrc name=src format=time block=true blocksize=8294400 max_bytes=8294400 caps=\"video/x-raw, width=1920, height=1080, format=RGBA, bpp=32, framerate=60/1, pixel-aspect-ratio=1/1\" "
      "! videoconvert "
      "! video/x-raw, format=I420, height=1080, width=1920 "
      "! x264enc bframes=0 key-int-max=0 "  //rc-lookahead=1 bitrate=1500 pass=quant tune=zerolatency
      "! video/x-h264 "// , stream-format=byte-stream, alignment=au 
      "! rtph264pay pt=96 mtu=1200 ssrc=42 config-interval=1 " //
      "! appsink name=sink "//sync=false
      // "! autovideosink"
      );

    e->bin = gst_parse_bin_from_description (video_desc, TRUE, &video_error);
    if (video_error) {
        printf ("video bin fail %s\n", video_error->message);
        return -1;
    }

    e->src = gst_bin_get_by_name (GST_BIN (e->bin), "src");
    g_assert(e->src);
    e->sink = gst_bin_get_by_name (GST_BIN (e->bin), "sink");
    g_assert(e->sink);

    
    gst_bin_add_many(GST_BIN(e->pipeline), e->bin, NULL);

    e->bus = gst_pipeline_get_bus(GST_PIPELINE(e->pipeline));
    gst_bus_add_signal_watch (e->bus);
    g_signal_connect (e->bus, "message", G_CALLBACK (bus_msg), GST_PIPELINE(e->pipeline));

    g_signal_connect (e->src, "need-data", G_CALLBACK (need_data), ctx);

    ret = gst_element_set_state (e->pipeline, GST_STATE_PLAYING);

    // ret = gst_element_get_state (e->pipeline, &state, NULL, GST_CLOCK_TIME_NONE);
    // if (ret == GST_STATE_CHANGE_FAILURE) {
    //   printf("Pipeline would not play\n");
    //   return -1; // or whatever cleanup you want
    // } else {
    //   printf("Pipeline is now in PLAYING state");
    // }

    e->loop = g_main_loop_new (NULL, FALSE);
    pthread_create(&e->main_thread, NULL, g_main_loop_run, e->loop);
    return 0;
}

static int gst_process_frame(struct ouvr_ctx *ctx, struct ouvr_packet *pkt)
{
    GstSample *sample;
    GstFlowReturn ret;
    gst_encode_context *e = ctx->enc_priv;

#ifdef UE4DEBUG
    printf("before emit pull-sample\n");
#endif
    g_signal_emit_by_name (e->sink, "pull-sample", &sample);
    if (sample) {
      // Actual compressed image is stored inside GstSample.
      GstBuffer *buffer = gst_sample_get_buffer(sample);
      GstMapInfo map;
      gst_buffer_map(buffer, &map, GST_MAP_READ);

      // Copy image
#ifdef GST_LOG
      printf("send pkt %d \n", map.size);
#endif
      memmove(pkt->data, map.data, map.size);
      pkt->size = map.size;
      gst_buffer_unmap(buffer, &map);
      gst_sample_unref(sample);

      return 1;
    } else {
        printf("return sample = nullptr\n");
        return -1;
    }

    return 0;
}

static void gst_deinitialize(struct ouvr_ctx *ctx)
{
    gst_encode_context *e = ctx->enc_priv;

    gst_object_unref (e->pipeline);
    gst_object_unref (e->bin);
    gst_object_unref(e->bus);

    free(e);
    ctx->enc_priv = NULL;
}

struct ouvr_encoder gst_encode = {
    .init = gst_initialize,
    .process_frame = gst_process_frame,
    .deinit = gst_deinitialize,
};
