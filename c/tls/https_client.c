/* Dowód TLS: HTTPS GET przez cosmo-mbedtls (2.26). */
#include <stdio.h>
#include <string.h>
#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#include "third_party/mbedtls/ssl.h"
#include "third_party/mbedtls/net_sockets.h"
#include "third_party/mbedtls/entropy.h"
#include "third_party/mbedtls/ctr_drbg.h"

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "api.opensubtitles.com";
    const char *path = argc > 2 ? argv[2] : "/api/v1/infos/formats";
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
    int r;

    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&ent);

    if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                   (const unsigned char *)"aqnapi", 6)) != 0) {
        printf("drbg_seed: -0x%04x\n", -r); return 1;
    }
    if ((r = mbedtls_net_connect(&net, host, "443", MBEDTLS_NET_PROTO_TCP)) != 0) {
        printf("connect: -0x%04x\n", -r); return 1;
    }
    if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
             MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        printf("config_defaults: -0x%04x\n", -r); return 1;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE); /* dowód: bez weryfikacji CA */
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0) { printf("setup: -0x%04x\n", -r); return 1; }
    if ((r = mbedtls_ssl_set_hostname(&ssl, host)) != 0) { printf("hostname: -0x%04x\n", -r); return 1; }
    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            printf("handshake: -0x%04x\n", -r); return 1;
        }
    }
    printf("TLS OK: %s / %s\n", mbedtls_ssl_get_version(&ssl),
           mbedtls_ssl_get_ciphersuite(&ssl));

    char req[512];
    snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: aqnapi v1.0.0\r\n"
        "Accept: application/json\r\nConnection: close\r\n\r\n", path, host);
    mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req));

    unsigned char buf[2048];
    int got = mbedtls_ssl_read(&ssl, buf, sizeof buf - 1);
    if (got > 0) {
        buf[got] = 0;
        /* pierwsza linia + ewentualny początek body */
        char *nl = strstr((char *)buf, "\r\n");
        if (nl) *nl = 0;
        printf("HTTP: %s\n", buf);
    } else {
        printf("read: -0x%04x\n", -got);
    }
    mbedtls_ssl_close_notify(&ssl);
    return 0;
}
