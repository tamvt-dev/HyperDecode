#include "../include/plugin.h"
#include "../include/logging.h"
#include <openssl/sha.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

static gboolean sha256_detect(Buffer in) {
    return in.len == 64 && g_ascii_isxdigit(in.data[0]); // hex digest
}

static Buffer sha256_encode(Buffer in) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(in.data, in.len, hash);

    char hex[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + i*2, "%02x", hash[i]);
    }
    hex[64] = 0;

    Buffer out;
    out.len = 64;
    out.data = g_malloc(64);
    if (out.data) {
        memcpy(out.data, hex, 64);
    }
    return out;
}

void sha256_plugin_init(void) {
    g_plugin_registry->register_plugin("SHA256",
                                       NULL,
                                       NULL,
                                       sha256_encode,
                                       sha256_detect,
                                       60);
    log_info("SHA256 plugin registered");
}
