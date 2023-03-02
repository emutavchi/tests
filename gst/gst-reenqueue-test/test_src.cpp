#include <glib.h>
#include <glib-unix.h>

#include <gst/gst.h>
#include <gst/base/gstflowcombiner.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <cmath>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cassert>

#include "test_src.h"

GST_DEBUG_CATEGORY(testsrc_debug);
#define GST_CAT_DEFAULT testsrc_debug

struct StreamData
{
    StreamData(std::vector<GstSample*> &&samples)
        : samples(std::move(samples))
    {
    }
    GstTestSrcPrivate *priv = 0;
    GstAppSrc* appsrc = 0;
    size_t sample_idx = 0;
    gboolean need_data = FALSE;
    gboolean got_enough_data = FALSE;
    std::string name;
    const std::vector<GstSample*> samples;
};

struct _GstTestSrcPrivate
{
    gchar* uri;
    guint pad_number;
    gboolean async_start;
    gboolean async_done;
    GstFlowCombiner* flowCombiner;
    std::vector<std::unique_ptr<StreamData>> streams;
};

enum { PROP_0, PROP_LOCATION };

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE("src_%u",
                            GST_PAD_SRC,
                            GST_PAD_SOMETIMES,
                            GST_STATIC_CAPS_ANY);

static void gst_test_src_uri_handler_init(gpointer gIface,
                                          gpointer ifaceData);

#define gst_test_src_parent_class parent_class

G_DEFINE_TYPE_WITH_CODE(
    GstTestSrc,
    gst_test_src,
    GST_TYPE_BIN,
    G_ADD_PRIVATE(GstTestSrc)
    G_IMPLEMENT_INTERFACE(
        GST_TYPE_URI_HANDLER,
        gst_test_src_uri_handler_init));

static void gst_test_src_init(GstTestSrc* src)
{
    GstTestSrcPrivate* priv = (GstTestSrcPrivate*)gst_test_src_get_instance_private(src);
    new (priv) GstTestSrcPrivate();
    src->priv = priv;
    src->priv->pad_number = 0;
    src->priv->async_start = FALSE;
    src->priv->async_done = FALSE;
    src->priv->flowCombiner = gst_flow_combiner_new();
    g_object_set(GST_BIN(src), "message-forward", TRUE, NULL);
}

static void gst_test_src_dispose(GObject* object)
{
    GST_CALL_PARENT(G_OBJECT_CLASS, dispose, (object));
}

static void gst_test_src_finalize(GObject* object)
{
    GstTestSrc* src = GST_TEST_SRC(object);
    GstTestSrcPrivate* priv = src->priv;

    g_free(priv->uri);
    gst_flow_combiner_free(priv->flowCombiner);
    priv->~GstTestSrcPrivate();

    GST_CALL_PARENT(G_OBJECT_CLASS, finalize, (object));
}

static void gst_test_src_set_property(GObject* object,
                                      guint propID,
                                      const GValue* value,
                                      GParamSpec* pspec)
{
    GstTestSrc* src = GST_TEST_SRC(object);

    switch (propID) {
        case PROP_LOCATION:
            gst_uri_handler_set_uri(reinterpret_cast<GstURIHandler*>(src),
                                    g_value_get_string(value), 0);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propID, pspec);
            break;
    }
}

static void gst_test_src_get_property(GObject* object,
                                      guint propID,
                                      GValue* value,
                                      GParamSpec* pspec)
{
    GstTestSrc* src = GST_TEST_SRC(object);
    GstTestSrcPrivate* priv = src->priv;

    GST_OBJECT_LOCK(src);
    switch (propID)
    {
        case PROP_LOCATION:
            g_value_set_string(value, priv->uri);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propID, pspec);
            break;
    }
    GST_OBJECT_UNLOCK(src);
}

// uri handler interface
static GstURIType gst_test_src_uri_get_type(GType)
{
    return GST_URI_SRC;
}

const gchar* const* gst_test_src_get_protocols(GType)
{
    static const char* protocols[] = {"testsrc", 0};
    return protocols;
}

static gchar* gst_test_src_get_uri(GstURIHandler* handler)
{
    GstTestSrc* src = GST_TEST_SRC(handler);
    gchar* ret;

    GST_OBJECT_LOCK(src);
    ret = g_strdup(src->priv->uri);
    GST_OBJECT_UNLOCK(src);
    return ret;
}

static gboolean gst_test_src_set_uri(GstURIHandler* handler,
                                     const gchar* uri,
                                     GError** error)
{
    GstTestSrc* src = GST_TEST_SRC(handler);
    GstTestSrcPrivate* priv = src->priv;

    if (GST_STATE(src) >= GST_STATE_PAUSED) {
        GST_ERROR_OBJECT(src, "URI can only be set in states < PAUSED");
        return FALSE;
    }

    GST_OBJECT_LOCK(src);

    g_free(priv->uri);
    priv->uri = 0;

    if (!uri) {
        GST_OBJECT_UNLOCK(src);
        return TRUE;
    }

    priv->uri = g_strdup(uri);
    GST_OBJECT_UNLOCK(src);
    return TRUE;
}

static void gst_test_src_uri_handler_init(gpointer gIface, gpointer)
{
    GstURIHandlerInterface* iface = (GstURIHandlerInterface*)gIface;

    iface->get_type = gst_test_src_uri_get_type;
    iface->get_protocols = gst_test_src_get_protocols;
    iface->get_uri = gst_test_src_get_uri;
    iface->set_uri = gst_test_src_set_uri;
}

static GstFlowReturn gst_test_src_chain_with_parent(GstPad* pad, GstObject* parent, GstBuffer* buffer)
{
    GstTestSrc* src = GST_TEST_SRC(gst_object_get_parent(parent));
    GstFlowReturn ret = gst_proxy_pad_chain_default(pad, GST_OBJECT(src), buffer);
    if (ret != GST_FLOW_FLUSHING) {
        ret = gst_flow_combiner_update_pad_flow(src->priv->flowCombiner, pad, ret);
    }
    gst_object_unref(src);
    return ret;
}

static gboolean gst_test_src_query_with_parent(GstPad* pad,
                                               GstObject* parent,
                                               GstQuery* query)
{
    gboolean result = FALSE;

    GstPad* target = gst_ghost_pad_get_target(GST_GHOST_PAD_CAST(pad));
    // Forward the query to the proxy target pad.
    if (target)
        result = gst_pad_query(target, query);
    gst_object_unref(target);

    return result;
}

static void gst_test_src_do_async_start(GstTestSrc* src)
{
    GstTestSrcPrivate* priv = src->priv;
    if (priv->async_done)
        return;
    priv->async_start = TRUE;
    GST_BIN_CLASS(parent_class)->handle_message(GST_BIN(src), gst_message_new_async_start(GST_OBJECT(src)));
}

static void gst_test_src_do_async_done(GstTestSrc* src)
{
    GstTestSrcPrivate* priv = src->priv;
    if (!priv->async_start)
        return;
    GST_BIN_CLASS(parent_class)->handle_message(GST_BIN(src), gst_message_new_async_done(GST_OBJECT(src), GST_CLOCK_TIME_NONE));
    priv->async_start = FALSE;
    priv->async_done = TRUE;
}

static GstStateChangeReturn gst_test_src_change_state(GstElement* element,
                                                      GstStateChange transition)
{
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    GstTestSrc* src = GST_TEST_SRC(element);
    GstTestSrcPrivate* priv = src->priv;

    switch (transition) {
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            gst_test_src_do_async_start(src);
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

    if (G_UNLIKELY(ret == GST_STATE_CHANGE_FAILURE)) {
        gst_test_src_do_async_done(src);
        return ret;
    }

    switch (transition) {
        case GST_STATE_CHANGE_READY_TO_PAUSED: {
            if (!priv->async_done)
                ret = GST_STATE_CHANGE_ASYNC;
            break;
        }
        case GST_STATE_CHANGE_PAUSED_TO_READY: {
            gst_test_src_do_async_done(src);
            break;
        }
        default:
            break;
    }

    return ret;
}

static void gst_test_src_class_init(GstTestSrcClass* klass)
{
    GST_DEBUG_CATEGORY_INIT(testsrc_debug, "testsrc", 0, "TestSrc");

    GObjectClass* oklass = G_OBJECT_CLASS(klass);
    GstElementClass* eklass = GST_ELEMENT_CLASS(klass);

    oklass->dispose = gst_test_src_dispose;
    oklass->finalize = gst_test_src_finalize;
    oklass->set_property = gst_test_src_set_property;
    oklass->get_property = gst_test_src_get_property;

    gst_element_class_add_pad_template(
        eklass, gst_static_pad_template_get(&src_template));
    g_object_class_install_property(
        oklass, PROP_LOCATION,
        g_param_spec_string(
            "location", "location", "Location to read from", 0,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    eklass->change_state = GST_DEBUG_FUNCPTR(gst_test_src_change_state);
}

static void gst_test_src_stream_push_samples(StreamData& data)
{
    assert(g_main_context_is_owner(g_main_context_default()));

    gboolean need_data;
    GstAppSrc* appsrc = data.appsrc;

    GST_OBJECT_LOCK(appsrc);
    need_data = std::exchange(data.need_data, FALSE);
    GST_OBJECT_UNLOCK(appsrc);

    if (need_data == FALSE)
        return;

    const auto& samples = data.samples;
    auto& sample_idx = data.sample_idx;

    GstClockTime beginPTS = GST_CLOCK_TIME_NONE;
    GstClockTime endPTS = GST_CLOCK_TIME_NONE;

    data.got_enough_data = FALSE;
    for (; sample_idx < samples.size(); ++sample_idx)
    {
        if (data.got_enough_data == TRUE)
            break;

        GstSample* sample = samples[sample_idx];
        GstCaps* caps = gst_sample_get_caps (sample);
        if (caps != NULL)
            gst_app_src_set_caps (appsrc, caps);

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstBuffer* copy = gst_buffer_copy_deep(buffer);
        GST_DEBUG("push sample(pts=%" GST_TIME_FORMAT ", dts=%" GST_TIME_FORMAT ") to app src", GST_TIME_ARGS(GST_BUFFER_PTS(copy)), GST_TIME_ARGS(GST_BUFFER_DTS(copy)));
        GstFlowReturn ret = gst_app_src_push_buffer(appsrc, copy);
        if (ret != GST_FLOW_OK)
            break;

        if (beginPTS == GST_CLOCK_TIME_NONE)
            beginPTS = GST_BUFFER_PTS(buffer);

        if (endPTS == GST_CLOCK_TIME_NONE)
            endPTS = GST_BUFFER_PTS(buffer);
        else
            endPTS = std::max(endPTS, GST_BUFFER_PTS(buffer));
    }

    if (beginPTS != GST_CLOCK_TIME_NONE)
        g_print("Enqueued %s [%" GST_TIME_FORMAT ", %" GST_TIME_FORMAT "]\n", data.name.c_str(), GST_TIME_ARGS(beginPTS), GST_TIME_ARGS(endPTS));
}

static void gst_test_src_appsrc_need_data(GstAppSrc* appsrc, guint, gpointer userData)
{
    StreamData& data = *(StreamData*) userData;

    gboolean post_message = FALSE;

    GST_OBJECT_LOCK(appsrc);
    if (data.need_data == FALSE) {
        data.need_data = TRUE;
        post_message = TRUE;
    }
    GST_OBJECT_UNLOCK(appsrc);

    if (post_message) {
        GstObject* src = gst_element_get_parent(appsrc);
        GstStructure* structure = gst_structure_new_empty("source-need-push");
        gst_element_post_message(GST_ELEMENT_CAST(src), gst_message_new_application(src, structure));
        gst_object_unref(src);
    }
}

static void gst_test_src_appsrc_enough_data(GstAppSrc*, gpointer userData)
{
    StreamData& data = *(StreamData*) userData;
    data.got_enough_data = TRUE;
}

static gboolean gst_test_src_appsrc_seek_data(GstAppSrc *appsrc, guint64 position, gpointer userData)
{
    assert(g_main_context_is_owner(g_main_context_default()));

    StreamData& data = *(StreamData*) userData;
    if (position == GST_CLOCK_TIME_NONE)
        return FALSE;

    const auto& samples = data.samples;
    auto it = std::lower_bound(samples.begin(), samples.end(), position, [](GstSample* sample, GstClockTime position) -> bool {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        return GST_BUFFER_PTS(buffer) < position;
    });

    if (it == samples.end()) {
        g_warning("No sample found for target time %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(position));
        return FALSE;
    }

    auto revit = std::find_if(std::make_reverse_iterator(it), samples.rend(), [] (GstSample* sample) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        return !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    });

    if (revit == samples.rend())
        data.sample_idx = 0;
    else
        data.sample_idx = std::distance(revit, samples.rend()) - 1;

    GstBuffer* buffer = gst_sample_get_buffer(samples[data.sample_idx]);
    assert(!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT));
    g_print("=== seek-data: found sync sample at idx=%zd pts=%" GST_TIME_FORMAT " for position %" GST_TIME_FORMAT "\n",
            data.sample_idx, GST_TIME_ARGS(GST_BUFFER_PTS(buffer)), GST_TIME_ARGS(position));

    return TRUE;
}

void gst_test_src_setup_add_stream(GstTestSrc* src, const char* name, std::vector<GstSample*> samples)
{
    if (samples.empty())
        return;

    GstTestSrcPrivate* priv = src->priv;

    static GstAppSrcCallbacks callbacks = {
        gst_test_src_appsrc_need_data,
        gst_test_src_appsrc_enough_data,
        gst_test_src_appsrc_seek_data,
        { 0 }
    };

    GstElement* appsrc = gst_element_factory_make("appsrc", name);

    StreamData *streamDataPtr = nullptr;
    {
        auto streamData = std::make_unique<StreamData>(std::move(samples));
        streamData->priv = priv;
        streamData->appsrc = GST_APP_SRC(appsrc);
        streamData->name = name;
        streamDataPtr = streamData.get();
        priv->streams.push_back(std::move(streamData));
    }

    g_object_set(appsrc, "block", FALSE, "format", GST_FORMAT_TIME, "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, nullptr);
    gst_app_src_set_callbacks(GST_APP_SRC(appsrc), &callbacks, streamDataPtr, nullptr);
    gst_app_src_set_max_bytes(GST_APP_SRC(appsrc), 16 * 1024 * 1024);

    gchar* pad_name = g_strdup_printf("src_%u", src->priv->pad_number);
    src->priv->pad_number++;
    gst_bin_add(GST_BIN(src), appsrc);
    GstPad* target = gst_element_get_static_pad(appsrc, "src");
    GstPad* pad = gst_ghost_pad_new(pad_name, target);

    auto proxypad = GST_PAD(gst_proxy_pad_get_internal(GST_PROXY_PAD(pad)));
    gst_flow_combiner_add_pad(priv->flowCombiner, proxypad);
    gst_pad_set_chain_function(proxypad, static_cast<GstPadChainFunction>(gst_test_src_chain_with_parent));
    gst_object_unref(proxypad);

    gst_pad_set_query_function(pad, gst_test_src_query_with_parent);
    gst_pad_set_active(pad, TRUE);

    gst_element_add_pad(GST_ELEMENT(src), pad);
    GST_OBJECT_FLAG_SET(pad, GST_PAD_FLAG_NEED_PARENT);

    gst_element_sync_state_with_parent(appsrc);

    g_free(pad_name);
    gst_object_unref(target);
}

void gst_test_src_setup_complete(GstTestSrc* src)
{
    gst_element_no_more_pads(GST_ELEMENT(src));
    gst_test_src_do_async_done(src);
}

void gst_test_src_push_samples(GstTestSrc* src)
{
    GstTestSrcPrivate* priv = src->priv;
    for (auto& s : priv->streams)
        gst_test_src_stream_push_samples(*s);
}

static GstPadProbeReturn gst_test_src_segment_fixer_probe(GstPad*, GstPadProbeInfo* info, gpointer)
{
    GstEvent* event = GST_EVENT(info->data);

    if (GST_EVENT_TYPE(event) != GST_EVENT_SEGMENT)
        return GST_PAD_PROBE_OK;

    GstSegment* segment = nullptr;
    gst_event_parse_segment(event, const_cast<const GstSegment**>(&segment));

    g_print("Fixed segment base time from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
            GST_TIME_ARGS(segment->base), GST_TIME_ARGS(segment->start));

    segment->base = segment->start;
    segment->flags = static_cast<GstSegmentFlags>(0);

    return GST_PAD_PROBE_REMOVE;
}

void gst_test_src_flush_and_reenqueue(GstTestSrc* src, const char* name, GstClockTime position)
{
    if (!GST_CLOCK_TIME_IS_VALID(position))
    {
        GST_WARNING("Invalid position, avoiding flush and re-enqueueing");
        return;
    }

    GstTestSrcPrivate* priv = src->priv;

    auto& streams = priv->streams;
    auto it = std::find_if(streams.begin(), streams.end(), [name](const auto& s) { return s->name == name; });
    if (it == streams.end())
        return;

    StreamData& data = *(it->get());
    GstAppSrc* appsrc = data.appsrc;

    // Query current segment
    double rate;
    GstFormat format;
    gint64 start = GST_CLOCK_TIME_NONE;
    gint64 stop = GST_CLOCK_TIME_NONE;

    {
        GstElement *pipeline = nullptr;
        for (GstElement* element = GST_ELEMENT_CAST(appsrc); element != nullptr; )
        {
            element = GST_ELEMENT_CAST(gst_element_get_parent(element));
            if (element)
            {
                if (pipeline)
                    gst_object_unref(pipeline);
                pipeline = element;
            }
        }
        GstQuery* query = gst_query_new_segment(GST_FORMAT_TIME);
        if (gst_element_query(pipeline, query))
            gst_query_parse_segment(query, &rate, &format, &start, &stop);
        gst_query_unref(query);
        if (pipeline)
            gst_object_unref(pipeline);
    }

    assert(static_cast<guint64>(start) != GST_CLOCK_TIME_NONE);

    GST_TRACE("segment: [%" GST_TIME_FORMAT ", %" GST_TIME_FORMAT "], rate: %f",
              GST_TIME_ARGS(start), GST_TIME_ARGS(stop), rate);

    // Start flush
    if (!gst_element_send_event(GST_ELEMENT(appsrc), gst_event_new_flush_start())) {
        GST_WARNING("Failed to send flush-start event for trackId=%s", name);
        return;
    }

    // Update segment
    GstSegment* segment = gst_segment_new();
    gst_segment_init(segment, GST_FORMAT_TIME);
    gst_segment_do_seek(segment, rate, GST_FORMAT_TIME, GST_SEEK_FLAG_NONE,
                        GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_SET, stop, nullptr);

    GstPad *srcPad, *sinkPad = nullptr;
    srcPad = gst_element_get_static_pad(GST_ELEMENT_CAST(appsrc), "src");
    assert(srcPad != nullptr);
    sinkPad = gst_pad_get_peer(srcPad);
    if (sinkPad) {
        gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, gst_test_src_segment_fixer_probe, nullptr, nullptr);
        gst_object_unref(sinkPad);
    }
    gst_object_unref(srcPad);

    if (!gst_base_src_new_seamless_segment(GST_BASE_SRC(appsrc), segment->start, segment->stop, segment->start)) {
        GST_WARNING("Failed to set seamless segment event for trackId=%s", data.name.c_str());
    } else {
        g_print("Set new seamless segment: [%" GST_TIME_FORMAT ", %" GST_TIME_FORMAT "], rate: %f \n",
                GST_TIME_ARGS(segment->start), GST_TIME_ARGS(segment->stop), segment->rate);
    }
    gst_segment_free(segment);

    // Stop flush
    if (!gst_element_send_event(GST_ELEMENT(appsrc), gst_event_new_flush_stop(false))) {
        GST_WARNING("Failed to send flush-stop event for trackId=%s", name);
        return;
    }
    GST_DEBUG("trackId=%s flushed", name);

    gst_test_src_appsrc_seek_data(appsrc, position, &data);
    gst_test_src_stream_push_samples(data);
}

void gst_test_src_end_of_stream(GstTestSrc* src)
{
    GstTestSrcPrivate* priv = src->priv;
    for (auto& s : priv->streams)
        gst_app_src_end_of_stream(s->appsrc);

    g_print("Marked EOS\n");
}
