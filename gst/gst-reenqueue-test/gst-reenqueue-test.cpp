#include <signal.h>
#include <glib.h>
#include <glib-unix.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cassert>

#include "test_src.h"

static bool keep_running = true;
static GMainLoop *loop;
static GstElement *playback_pipeline;

struct SourceData
{
    GstClockTime highPTS  = 0;
    GstClockTime bufLimit = GST_CLOCK_TIME_NONE;
    GstClockTime sampleDur= GST_CLOCK_TIME_NONE;
    std::vector<GstSample*> samples;
};

struct PlaybackData
{
    bool did_start = false;
    bool did_mark_eos = false;

    GstClockTime duration = GST_CLOCK_TIME_NONE;
    GstClockTime reenqueue_position = GST_CLOCK_TIME_NONE;
    GstClockTime prev_position = GST_CLOCK_TIME_NONE;
    GstClockTime reenqueue_position_on_sink_paused = GST_CLOCK_TIME_NONE;

    GstTestSrc*  source = nullptr;
    std::vector<std::vector<GstSample*>> streamSamples;
};

static void print_position_per_sink(GstElement* element)
{
  auto fold_func = [](const GValue *vitem, GValue*, gpointer) -> gboolean {
    GstObject *item = GST_OBJECT(g_value_get_object (vitem));
    if (GST_IS_BIN (item)) {
      print_position_per_sink(GST_ELEMENT(item));
    }
    else if (GST_IS_BASE_SINK(item)) {
      GstElement* el = GST_ELEMENT(item);
      GstQuery* query = gst_query_new_position(GST_FORMAT_TIME);
      if (gst_element_query(el, query)) {
        gint64 position = GST_CLOCK_TIME_NONE;
        gst_query_parse_position(query, 0, &position);
        g_print("Position from %s : %" GST_TIME_FORMAT "\n", GST_ELEMENT_NAME(el), GST_TIME_ARGS(position));
      }
      gst_query_unref(query);
    }
    return TRUE;
  };

  GstBin *bin = GST_BIN_CAST (element);
  GstIterator *iter = gst_bin_iterate_sinks (bin);

  bool keep_going = true;
  while (keep_going) {
    GstIteratorResult ires;
    ires = gst_iterator_fold (iter, fold_func, NULL, NULL);
    switch (ires) {
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      default:
        keep_going = false;
        break;
    }
  }
  gst_iterator_free (iter);
}

static gboolean has_paused_sink_state(GstElement* element)
{
  auto fold_func = [](const GValue *vitem, GValue* ret, gpointer) -> gboolean {
    GstObject *item = GST_OBJECT(g_value_get_object (vitem));
    if (GST_IS_BIN (item)) {
        if (has_paused_sink_state(GST_ELEMENT(item))) {
            g_value_set_boolean (ret, TRUE);
            return FALSE;
        }
    }
    else if (GST_IS_ELEMENT(item)){
        GstState state;
        gst_element_get_state(GST_ELEMENT(item), &state, NULL, 0);
        if (state != GST_STATE_PLAYING) {
            g_value_set_boolean (ret, TRUE);
            return FALSE;
        }
    }
    return TRUE;
  };

  GValue ret = G_VALUE_INIT;
  GstBin *bin = GST_BIN_CAST (element);
  GstIterator *iter = gst_bin_iterate_sinks (bin);

  g_value_init (&ret, G_TYPE_BOOLEAN);
  g_value_set_boolean (&ret, FALSE);


  bool keep_going = true;
  while (keep_going) {
    GstIteratorResult ires = gst_iterator_fold (iter, fold_func, &ret, NULL);
    if (GST_ITERATOR_RESYNC == ires) {
        gst_iterator_resync (iter);
        continue;
    }
    break;
  }
  gst_iterator_free (iter);

  return g_value_get_boolean(&ret);
}

gboolean areAllPlaying(GstBin* bin)
{
    static auto iterateSinks = [](GstBin* bin, GstIteratorFoldFunction foldFunc) -> gboolean {
        GValue ret = G_VALUE_INIT;
        g_value_init (&ret, G_TYPE_BOOLEAN);
        g_value_set_boolean (&ret, TRUE);
        GstIterator *iter = gst_bin_iterate_sinks(bin);
        for (;;) {
            auto res = gst_iterator_fold (iter, foldFunc, &ret, nullptr);
            if (GST_ITERATOR_RESYNC == res) {
                gst_iterator_resync (iter);
                continue;
            }
            break;
        }
        gst_iterator_free(iter);
        return g_value_get_boolean(&ret);
    };

    static GstIteratorFoldFunction foldFunc = [](const GValue *vitem, GValue* ret, gpointer) -> gboolean {
        GstObject *item = GST_OBJECT(g_value_get_object (vitem));
        if (GST_IS_ELEMENT(item)) {
            GstState state, pending;
            gst_element_get_state(GST_ELEMENT(item), &state, &pending, 0);
            if (state != GST_STATE_PLAYING && pending != GST_STATE_PLAYING) {
                g_value_set_boolean (ret, FALSE);
                return FALSE;
            }
            if (GST_IS_BIN (item)) {
                if (iterateSinks(GST_BIN(item), (GstIteratorFoldFunction)foldFunc) == FALSE) {
                    g_value_set_boolean (ret, FALSE);
                    return FALSE;
                }
            }
        }
        return TRUE;
    };

    return iterateSinks(bin, foldFunc);
}

static void reenqueue(GstClockTime position, gpointer userData)
{
    PlaybackData& data = *(PlaybackData*) userData;
    if (!data.did_start || !GST_CLOCK_TIME_IS_VALID(position))
        return;

    g_print("=== Re-enqueue at %" GST_TIME_FORMAT " ===\n", GST_TIME_ARGS(position));

    gst_test_src_flush_and_reenqueue(data.source, "video", position);
}

static gboolean check_progress(gpointer userData)
{
    PlaybackData& data = *(PlaybackData*) userData;
    if (!data.did_start)
        return G_SOURCE_CONTINUE;

    GstClockTime position = GST_CLOCK_TIME_NONE;
    GstQuery* query = gst_query_new_position(GST_FORMAT_TIME);
    if (gst_element_query(playback_pipeline, query))
    {
        gint64 tmp;
        gst_query_parse_position(query, 0, &tmp);
        position = tmp;
    }
    else
    {
        GST_DEBUG("Position query failed\n");
    }
    gst_query_unref(query);

    if (!GST_CLOCK_TIME_IS_VALID(position)) {
      GST_DEBUG("Position is not valid\n");
      return G_SOURCE_CONTINUE;
    }

    auto abs_time_diff = [](GstClockTime t1, GstClockTime t2) -> GstClockTimeDiff
    {
      if (t1 > t2)
        return t1 - t2;
      return t2 - t1;
    };

    GstClockTime prev_position = std::exchange(data.prev_position, position);

    if (GST_CLOCK_TIME_IS_VALID(prev_position) && abs_time_diff(position, prev_position) > 350 * GST_MSECOND)
    {
        g_warning("Unexpected position!!! more than 350ms jump detected. prev = %" GST_TIME_FORMAT " --> curr = %" GST_TIME_FORMAT "!!!\n",
                  GST_TIME_ARGS(prev_position),
                  GST_TIME_ARGS(position));
        print_position_per_sink(playback_pipeline);
    }
    else
    {
        if (!data.did_mark_eos)
        {
            if (abs_time_diff(position, data.duration) < GST_SECOND)
            {
                gst_test_src_end_of_stream(data.source);
                data.did_mark_eos = true;
                data.reenqueue_position_on_sink_paused = GST_CLOCK_TIME_NONE;
            }
            else if (position > data.reenqueue_position)
            {
                data.reenqueue_position_on_sink_paused = position;
                data.reenqueue_position += 2 * GST_SECOND;

                reenqueue(position, userData);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean print_progress(gpointer userData)
{
    PlaybackData& data = *(PlaybackData*) userData;
    if (!data.did_start)
        return G_SOURCE_CONTINUE;

    #if 1
    GstQuery* query = gst_query_new_position(GST_FORMAT_TIME);
    if (gst_element_query(playback_pipeline, query))
    {
        gint64 position = GST_CLOCK_TIME_NONE;
        gst_query_parse_position(query, 0, &position);
        g_print("Position: %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(position));
    }
    gst_query_unref(query);
    #else
    print_position_per_sink(playback_pipeline);
    #endif

    return G_SOURCE_CONTINUE;
}

static void async_done(gpointer userData)
{
    PlaybackData& data = *(PlaybackData*) userData;
    if (!data.did_start)
    {
        data.did_start = true;
        gst_element_set_state(playback_pipeline, GST_STATE_PLAYING);
    }
}

static void bus_message_callback(GstBus*, GstMessage* message, gpointer userData)
{
    gchar* debug = nullptr;
    switch (GST_MESSAGE_TYPE(message))
    {
        case GST_MESSAGE_ERROR:
        {
            gst_message_parse_error(message, nullptr, &debug);
            g_print("Got error message '%s'\n", debug);
            g_free(debug);
            keep_running = false;
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_EOS:
        {
            g_print("Got EOS\n");
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_APPLICATION:
        {
            const GstStructure* structure = gst_message_get_structure(message);
            if (gst_structure_has_name(structure, "source-need-push"))
            {
                PlaybackData& data = *(PlaybackData*) userData;
                assert(data.source == GST_TEST_SRC(GST_MESSAGE_SRC(message)));
                gst_test_src_push_samples(data.source);
            }
            break;
        }
        case GST_MESSAGE_STATE_CHANGED:
        {
            GstElement* element = GST_ELEMENT(GST_MESSAGE_SRC(message));
            GstState state, pending;
            GstStateChangeReturn ret = gst_element_get_state(element, &state, &pending, 0);

            g_print("### %s state changed, change return: %s, state: %s, pending: %s\n",
                    GST_MESSAGE_SRC_NAME(message),
                    gst_element_state_change_return_get_name(ret),
                    gst_element_state_get_name(state),
                    gst_element_state_get_name(pending));

            if (GST_MESSAGE_SRC(message) == GST_OBJECT(playback_pipeline)
                && state == GST_STATE_PLAYING
                && pending == GST_STATE_VOID_PENDING)
            {
                if (areAllPlaying(GST_BIN(playback_pipeline)) == FALSE)
                {
                    g_print("!!! Detected pipeline in playing state with sink in paused state !!!\n");
                    gchar* dotFileName = g_strdup_printf(
                        "%s.%s-%s",
                        GST_OBJECT_NAME(playback_pipeline),
                        gst_element_state_get_name(state),
                        gst_element_state_get_name(pending));
                    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(playback_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName);
                    g_free(dotFileName);
                }
            }
            else if (GST_IS_BASE_SINK(element) && state == GST_STATE_PAUSED && pending == GST_STATE_VOID_PENDING)
            {
                PlaybackData& data = *(PlaybackData*) userData;
                if (GST_CLOCK_TIME_IS_VALID(data.reenqueue_position_on_sink_paused))
                {
                    auto position = std::exchange(data.reenqueue_position_on_sink_paused, GST_CLOCK_TIME_NONE);
                    reenqueue(position, userData);
                }
            }
            break;
        }
        case GST_MESSAGE_ASYNC_DONE: {
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(playback_pipeline))
                async_done(userData);
            break;
        }
        default:
            break;
    }
}

static void setup_element(GstElement* pipeline, GstElement* element, gpointer userData)
{
    if (GST_IS_BASE_SINK(element))
    {
        g_object_set(G_OBJECT(element), "async", TRUE, nullptr);
    }
}

static void setup_source(GstElement* pipeline, GstElement* element, gpointer userData)
{
    PlaybackData& data = *(PlaybackData*) userData;
    data.source = GST_TEST_SRC(element);
    g_idle_add([](gpointer userData) {
        PlaybackData& data = *(PlaybackData*) userData;
        for(const auto& stream : data.streamSamples) {
            if (stream.empty())
                continue;
            const char* name = "unknown";

            GstCaps *caps = gst_sample_get_caps(stream.front());
            gchar *caps_str = gst_caps_to_string(caps);
            if (g_str_has_prefix(caps_str, "video/"))
                name = "video";
            else if (g_str_has_prefix(caps_str, "audio/"))
                name = "audio";
            g_free(caps_str);

            gst_test_src_setup_add_stream(data.source, name, stream);
        }
        gst_test_src_setup_complete(data.source);
        return G_SOURCE_REMOVE;
    }, userData);
}

static GstElement* create_playback_pipeline(PlaybackData* data)
{
    GstElement* pipeline = gst_element_factory_make("playbin", nullptr);
    if (!pipeline)
    {
        g_warning("Failed to create pipeline\n");
        g_main_loop_quit(loop);
        keep_running = false;
        return nullptr;
    }

    g_signal_connect(pipeline,  "element-setup", G_CALLBACK(setup_element), data);
    g_signal_connect(pipeline, "source-setup", G_CALLBACK(setup_source), data);
    g_object_set(pipeline, "uri", "testsrc://", "message-forward", FALSE, nullptr);

    GstElement* playsink = gst_bin_get_by_name(GST_BIN(pipeline), "playsink");
    g_object_set(G_OBJECT(playsink), "send-event-mode", 0, nullptr);
    g_object_unref(playsink);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);

    return pipeline;
}

static gboolean autoplug_continue(GstElement* element, GstPad* pad, GstCaps* caps, gpointer)
{
    gboolean result = FALSE;
    gchar* caps_str = gst_caps_to_string(caps);
    if (g_str_has_prefix(caps_str, "video/webm") ||
        g_str_has_prefix(caps_str, "audio/webm") ||
        g_str_has_prefix(caps_str, "video/quicktime") ||
        g_str_has_prefix(caps_str, "audio/quicktime") ||
        g_str_has_prefix(caps_str, "video/x-matroska") ||
        g_str_has_prefix(caps_str, "audio/x-matroska")) {
        result = TRUE;
    }
    g_print("got caps '%s', %s autoplug\n", caps_str, result ? "continue" : "stop");

    g_free(caps_str);
    return result;
}

static void src_bus_message_callback (GstBus*, GstMessage* message, gpointer)
{
    gchar* debug = nullptr;
    switch (GST_MESSAGE_TYPE(message))
    {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(message, nullptr, &debug);
            g_print("Got error message '%s'\n", debug);
            g_free(debug);
            keep_running = false;
            // FALLTHROUGH
        case GST_MESSAGE_EOS:
            g_print("Quit src\n");
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }
}

static GstFlowReturn app_sink_new_sample(GstAppSink *appsink, gpointer userData)
{
    auto &data = *(SourceData*)userData;

    GstSample* sample = gst_app_sink_pull_sample(appsink);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer)
        return GST_FLOW_OK;

    if (GST_CLOCK_TIME_IS_VALID(data.bufLimit) && data.highPTS > data.bufLimit)
    {
        GstAppSinkCallbacks null_callbacks = { 0, 0, 0, { 0 } };
        gst_app_sink_set_callbacks(appsink, &null_callbacks, 0, 0);
        gst_app_sink_set_drop(appsink, TRUE);
        gst_app_sink_set_max_buffers(appsink, 1);
        gst_sample_unref(sample);
        return GST_FLOW_EOS;
    }

    assert(GST_BUFFER_PTS_IS_VALID(buffer) || GST_BUFFER_DTS_IS_VALID(buffer));

    if (!GST_BUFFER_PTS_IS_VALID(buffer))
    {
        #if GST_CHECK_VERSION(1, 16, 0)
        sample = gst_sample_make_writable(sample);
        assert(gst_sample_is_writable(sample));
        buffer = gst_buffer_make_writable(gst_buffer_ref(buffer));
        assert(gst_buffer_is_writable(buffer));
        gst_sample_set_buffer(sample, buffer);
        #endif

        GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer);
    }

    if (data.highPTS == GST_CLOCK_TIME_NONE)
        data.highPTS = GST_BUFFER_PTS(buffer);
    else
        data.highPTS = std::max(data.highPTS, GST_BUFFER_PTS(buffer));

    data.samples.push_back(sample);
    return GST_FLOW_OK;
}

static GstElement* create_buffering_pipeline(const char* uri, SourceData *data)
{
    GError *error = 0;
    const gchar* launch_str = "uridecodebin name=in ! appsink name=out";
    GstElement *src_pipeline = gst_parse_launch(launch_str, &error);

    if (!src_pipeline)
    {
        g_warning("Failed to create buffering pipeline\n");
        if (error)
        {
            g_warning("Parse error: '%s'", error->message);
            g_error_free(error);
        }
        return nullptr;
    }

    GstElement *src = gst_bin_get_by_name (GST_BIN(src_pipeline), "in");
    GstElement *out = gst_bin_get_by_name (GST_BIN(src_pipeline), "out");
    g_object_set(src, "uri", uri, nullptr);
    g_signal_connect(src, "autoplug-continue", G_CALLBACK(autoplug_continue), 0);

    gst_base_sink_set_sync(GST_BASE_SINK(out), FALSE);
    gst_app_sink_set_emit_signals(GST_APP_SINK(out), FALSE);
    gst_app_sink_set_wait_on_eos(GST_APP_SINK(out), FALSE);

    static GstAppSinkCallbacks appsink_callbacks = { 0, 0, app_sink_new_sample, { 0 } };
    gst_app_sink_set_callbacks(GST_APP_SINK(out), &appsink_callbacks, data, 0);

    gst_element_set_state(src_pipeline, GST_STATE_PLAYING);

    gst_object_unref(src);
    gst_object_unref(out);
    return src_pipeline;
}

static gboolean quit_handler(gpointer user_data)
{
    g_print ("Quit !!!\n");
    keep_running = false;
    g_main_loop_quit (loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    signal (SIGPIPE, SIG_IGN);
    g_unix_signal_add (SIGINT, quit_handler, NULL);
    g_unix_signal_add (SIGTERM, quit_handler, NULL);

    gdouble reenqueue_at = 2.0;
    gdouble max_buf = 20.0;
    GError *error = nullptr;
    GOptionContext *optionCtx = nullptr;
    GOptionEntry options[] = {
        { "re-enqueue-at", 'r', 0, G_OPTION_ARG_DOUBLE, &reenqueue_at, "Time of initial re-enqueue (default 2.0 second)", NULL },
        { "max-buf", 'm', 0, G_OPTION_ARG_DOUBLE, &max_buf, "Limit the source buffer size", NULL },
        { NULL }
    };

    optionCtx = g_option_context_new ("<uri> [<uri>]");
    g_option_context_add_main_entries(optionCtx, options, NULL);
    g_option_context_add_group(optionCtx, gst_init_get_option_group());

    if (!g_option_context_parse(optionCtx, &argc, &argv, &error))
    {
        g_print ("Error initializing: %s\n", error ? error->message : nullptr);
        return -1;
    }

    if (!gst_init_check(&argc, &argv, &error))
    {
        g_print("GST init fail: %s\n", error ? error->message : nullptr);
        g_error_free (error);
        return -1;
    }

    std::vector<std::string> uris;
    for (int i = 1; i < argc; ++i)
    {
        char* uri;

        if (g_strrstr(argv[i], "://") == nullptr)
        {
            GError *error = 0;
            uri = g_filename_to_uri(argv[i], nullptr, &error);
            if (uri == nullptr)
            {
                g_print("Couldn't build uri: %s\n", error ? error->message : nullptr);
                g_error_free (error);
                continue;
            }
        }
        else
        {
            uri = g_strdup(argv[i]);
        }

        uris.push_back(uri);
        g_free(uri);
    }

    if (uris.empty())
    {
        g_print("%s", g_option_context_get_help(optionCtx, TRUE, nullptr));
        return -1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    PlaybackData playbackData;
    {
        g_print("Buffering \n");

        for(const auto& uri : uris)
        {
            SourceData sourceData;
            sourceData.bufLimit = max_buf * GST_SECOND;

            GstElement* buff_pipeline = create_buffering_pipeline(uri.c_str(), &sourceData);
            if (buff_pipeline) {
                GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(buff_pipeline));
                gst_bus_add_signal_watch_full(bus, G_PRIORITY_HIGH);
                g_signal_connect(bus, "message", G_CALLBACK(src_bus_message_callback), &sourceData);

                g_main_loop_run(loop);

                gst_bus_remove_signal_watch(bus);
                gst_element_set_state(buff_pipeline, GST_STATE_NULL);
                gst_object_unref(bus);
                gst_object_unref(buff_pipeline);
            }

            if (sourceData.samples.size()) {
              g_print("Done buffering %s [%" GST_TIME_FORMAT ", %" GST_TIME_FORMAT ")\n"
                        , uri.c_str()
                        , GST_TIME_ARGS(GST_BUFFER_PTS(gst_sample_get_buffer(sourceData.samples.front())))
                        , GST_TIME_ARGS(GST_BUFFER_PTS(gst_sample_get_buffer(sourceData.samples.back()))));
            } else {
                g_warning("no samples from %s\n", uri.c_str());
                continue;
            }

            if (playbackData.duration == GST_CLOCK_TIME_NONE)
                playbackData.duration = sourceData.highPTS;
            else
                playbackData.duration = std::min(playbackData.duration, sourceData.highPTS);

            playbackData.streamSamples.push_back(std::move(sourceData.samples));
        }

        g_print("Buffered %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(playbackData.duration));
    }

    // Align streams
    for (auto& s : playbackData.streamSamples) {
        while (!s.empty()) {
            GstBuffer* buf = gst_sample_get_buffer(s.back());
            if (GST_BUFFER_PTS(buf) <= playbackData.duration)
                break;
            s.pop_back();
        }
    }

    gst_element_register(0, "testsrc", GST_RANK_PRIMARY + 100, GST_TEST_TYPE_SRC);

    g_timeout_add(16, check_progress, &playbackData);
    g_timeout_add(500, print_progress, &playbackData);

    while (keep_running && !playbackData.streamSamples.empty())
    {
        playbackData.reenqueue_position = reenqueue_at * GST_SECOND;
        playbackData.prev_position = GST_CLOCK_TIME_NONE;
        playbackData.source = nullptr;
        playbackData.did_start = false;
        playbackData.did_mark_eos = false;
        playbackData.reenqueue_position_on_sink_paused = GST_CLOCK_TIME_NONE;

        g_print("Starting playback: reenqueue position = %" GST_TIME_FORMAT " \n", GST_TIME_ARGS(playbackData.reenqueue_position));

        playback_pipeline = create_playback_pipeline(&playbackData);

        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(playback_pipeline));
        gst_bus_add_signal_watch_full(bus, G_PRIORITY_HIGH);
        g_signal_connect(bus, "message", G_CALLBACK(bus_message_callback), &playbackData);

        g_main_loop_run(loop);

        gst_bus_remove_signal_watch(bus);
        gst_element_set_state(playback_pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(playback_pipeline);
    }

    g_main_loop_unref(loop);
    gst_deinit();

    return 0;
}
