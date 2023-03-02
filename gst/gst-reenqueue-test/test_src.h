#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TEST_TYPE_SRC (gst_test_src_get_type())
#define GST_TEST_SRC(obj)                                               \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TEST_TYPE_SRC, GstTestSrc))
#define GST_TEST_SRC_CLASS(obj)                                         \
    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TEST_TYPE_SRC, GstTestSrcClass))
#define GST_IS_COLABT_SRC(obj)                              \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TEST_TYPE_SRC))
#define GST_IS_TEST_SRC_CLASS(klass)                        \
    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TEST_TYPE_SRC))

typedef struct _GstTestSrc GstTestSrc;
typedef struct _GstTestSrcClass GstTestSrcClass;
typedef struct _GstTestSrcPrivate GstTestSrcPrivate;

struct _GstTestSrc {
    GstBin parent;
    GstTestSrcPrivate* priv;
};

struct _GstTestSrcClass {
    GstBinClass parentClass;
};

GType gst_test_src_get_type(void);
void gst_test_src_setup_add_stream(GstTestSrc* src, const char* name, std::vector<GstSample*> samples);
void gst_test_src_setup_complete(GstTestSrc* src);
void gst_test_src_push_samples(GstTestSrc* src);
void gst_test_src_flush_and_reenqueue(GstTestSrc* src, const char* name, GstClockTime position);
void gst_test_src_end_of_stream(GstTestSrc* src);

G_END_DECLS
