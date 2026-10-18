// unit_aws_edge.c - the aws provider's edge purge (provider_aws.c
// aws_edge_purge, cbase/provider.h edge_purge) against a local TLS server
// standing in for CloudFront: with no distribution configured (a first
// deploy) it sends nothing and answers CB_OK; with one it sends one
// CreateInvalidation and returns what CloudFront answered.
#include "t.h"
#include "cbase/provider.h"

#include "aws/https.h"
#include "config.h"
#include "lib_aws_tls.h"
#include "util/err.h"
#include "util/log.h"

#include <stdlib.h>

static int g_status = 201;

static tls_action answer(const cb_buf *req, cb_buf *resp, void *ud) {
    cb_buf_printf(resp, "HTTP/1.1 %d X\r\nContent-Length: 0\r\n\r\n", g_status);
    return TLS_KEEP;
}

static void purge_with_no_distribution(void) {
    setenv("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", 1);
    setenv("AWS_SESSION_TOKEN", "SESSIONTOKEN", 1);
    setenv("AWS_REGION", "us-east-1", 1);
    tls_srv s;
    T_EQ_INT(tls_srv_start(&s, answer, NULL), 0);
    T_EQ_INT(cb_https_set_ca_file(s.ca_file), CB_OK);
    char ep[64];
    snprintf(ep, sizeof ep, "https://localhost:%d", s.port);
    setenv("AWS_ENDPOINT_URL_CLOUDFRONT", ep, 1);
    T_CHECK(cb_provider_aws.edge_purge != NULL);
    T_CHECK(cb_provider_aws.flags & CB_PROV_EDGE_PURGE);
    const char *paths[2] = { "/v1/a", "/v1/b" };
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    // No distribution, no config: CB_OK, nothing sent.
    T_EQ_INT(cb_provider_aws.edge_purge(&cfg, paths, 2), CB_OK);
    T_EQ_INT(cb_provider_aws.edge_purge(NULL, paths, 2), CB_OK);
    T_EQ_INT(atomic_load(&s.requests), 0);
    // With one: one request, to that distribution, for those paths.
    snprintf(cfg.cf_distribution, sizeof cfg.cf_distribution, "E2ABCDEF123456");
    T_EQ_INT(cb_provider_aws.edge_purge(&cfg, paths, 2), CB_OK);
    T_EQ_INT(atomic_load(&s.requests), 1);
    char *req = tls_srv_last(&s);
    T_CHECK(cb_starts_with(req, "POST /2020-05-31/distribution/E2ABCDEF123456/invalidation HTTP/1.1\r\n"));
    T_CHECK(strstr(req, "<Path>/v1/a</Path><Path>/v1/b</Path>") != NULL);
    free(req);
    // CloudFront refusing it is the purge's error.
    g_status = 403;
    T_EQ_INT(cb_provider_aws.edge_purge(&cfg, paths, 1), CB_E_IO);
    T_EQ_INT(atomic_load(&s.requests), 2);
    // And the distribution cleared again: still nothing more sent.
    cfg.cf_distribution[0] = 0;
    T_EQ_INT(cb_provider_aws.edge_purge(&cfg, paths, 1), CB_OK);
    T_EQ_INT(atomic_load(&s.requests), 2);
    unsetenv("AWS_ENDPOINT_URL_CLOUDFRONT");
    tls_srv_stop(&s);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    T_RUN(purge_with_no_distribution);
    return T_DONE();
}
