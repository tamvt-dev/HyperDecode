#include "../include/plugin.h"
#include "../include/logging.h"
#include <openssl/evp.h>
#include <string.h>
#include <glib.h>

static gboolean aes_detect(Buffer in) {
    // Detect AES-like (multiples of 16 bytes)
    return in.len % 16 == 0 && in.len >= 16;
}

static Buffer aes_decode_single(Buffer in) {
    unsigned char key[] = "1234567890123456"; // 128-bit fixed key
    if (in.len % 16 != 0 || in.len == 0) {
        Buffer out = { NULL, NULL, 0 };
        return out;
    }

    Buffer out_buf = { NULL, NULL, 0 };
    out_buf.data = g_malloc(in.len);
    out_buf.len  = in.len;
    if (!out_buf.data) {
        return out_buf;
    }

    unsigned char *out_ptr = out_buf.data;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        g_free(out_buf.data);
        return (Buffer){ NULL, NULL, 0 };
    }

    int len;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        g_free(out_buf.data);
        return (Buffer){ NULL, NULL, 0 };
    }

    size_t num_blocks = in.len / 16;
    for (size_t b = 0; b < num_blocks; ++b) {
        if (EVP_DecryptUpdate(ctx, out_ptr, &len, &in.data[b*16], 16) != 1) {
            break;
        }
        out_ptr += len;
    }

    EVP_CIPHER_CTX_free(ctx);
    return out_buf;
}

static Buffer aes_encode(Buffer in) {
    unsigned char key[] = "1234567890123456"; // 128-bit fixed key

    // PKCS7 padding
    size_t padded_len = ((in.len + 15) / 16) * 16;
    unsigned char *padded_data = g_malloc(padded_len);
    if (!padded_data) {
        return (Buffer){ NULL, NULL, 0 };
    }
    memcpy(padded_data, in.data, in.len);
    unsigned char pad_len = (unsigned char)(padded_len - in.len);
    memset(padded_data + in.len, pad_len, pad_len);

    Buffer out_buf = { NULL, NULL, 0 };
    out_buf.data = g_malloc(padded_len);
    out_buf.len  = padded_len;
    if (!out_buf.data) {
        g_free(padded_data);
        return (Buffer){ NULL, NULL, 0 };
    }

    unsigned char *out_ptr = out_buf.data;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        g_free(padded_data);
        g_free(out_buf.data);
        return (Buffer){ NULL, NULL, 0 };
    }

    int len;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        g_free(padded_data);
        g_free(out_buf.data);
        return (Buffer){ NULL, NULL, 0 };
    }

    size_t num_blocks = padded_len / 16;
    for (size_t b = 0; b < num_blocks; ++b) {
        if (EVP_EncryptUpdate(ctx, out_ptr, &len, &padded_data[b * 16], 16) != 1) {
            break;
        }
        out_ptr += len;
    }

    EVP_CIPHER_CTX_free(ctx);
    g_free(padded_data);
    return out_buf;
}

void aes_plugin_init(void) {
    g_plugin_registry->register_plugin("AES",
                                       aes_decode_single,
                                       NULL,  // no multi-decode
                                       aes_encode,
                                       aes_detect,
                                       80);
    log_info("AES plugin registered");
}
