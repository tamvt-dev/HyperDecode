#include "../include/plugin.h"
#include "../include/logging.h"
#include <zlib.h>
#include <glib.h>
#include <string.h>

static gboolean gzip_detect(Buffer in) {
    return in.len > 10 && in.data[0] == 0x1f && in.data[1] == 0x8b;
}

static Buffer gzip_decode(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    
    // 15 + 32 allows automatic header detection of both zlib and gzip formats
    if (inflateInit2(&strm, 15 + 32) != Z_OK) {
        return out;
    }

    strm.avail_in = in.len;
    strm.next_in = in.data;

    // Use a GByteArray to dynamically grow the output buffer
    GByteArray *out_arr = g_byte_array_new();
    unsigned char outbuf[8192];

    int ret;
    do {
        strm.avail_out = sizeof(outbuf);
        strm.next_out = outbuf;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            break;
        }
        
        size_t have = sizeof(outbuf) - strm.avail_out;
        if (have > 0) {
            g_byte_array_append(out_arr, outbuf, have);
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    if (ret == Z_STREAM_END && out_arr->len > 0) {
        out.len = out_arr->len;
        out.data = g_malloc(out.len);
        if (out.data) {
            memcpy(out.data, out_arr->data, out.len);
        } else {
            out.len = 0;
        }
    }

    g_byte_array_free(out_arr, TRUE);
    return out;
}

void gzip_plugin_init(void) {
    g_plugin_registry->register_plugin("GZip",
                                       gzip_decode,
                                       NULL,
                                       NULL,
                                       gzip_detect,
                                       90);
    log_info("GZip plugin registered");
}
