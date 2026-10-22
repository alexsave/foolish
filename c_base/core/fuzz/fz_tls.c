// fz_tls.c - the server's TLS (net/tls.h) reading what a client sends: the bytes of the input
// arrive on a socket the server side wraps with cb_tls_conn_new, and cb_tls_read and cb_tls_write
// run until OpenSSL has nothing more. What is under attack is the handshake and record parsing
// behind the server's listener, and the wrapper's outcomes: a read never reports more bytes than
// its buffer, and every result is bytes, 0, WANT or FAIL.
#include "fz.h"

#include "net/tls.h"
#include "util/err.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <unistd.h>

static cb_tls *T;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    char dir[] = "/tmp/fz-tls-XXXXXX", cert[64], key[64];
    if (!mkdtemp(dir)) abort();
    snprintf(cert, sizeof cert, "%s/c.pem", dir);
    snprintf(key, sizeof key, "%s/k.pem", dir);
    EVP_PKEY *k = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, k);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC, (const unsigned char *)"fz", -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(x));
    if (X509_sign(x, k, EVP_sha256()) <= 0) abort();
    FILE *f = fopen(cert, "w");
    PEM_write_X509(f, x);
    fclose(f);
    f = fopen(key, "w");
    PEM_write_PrivateKey(f, k, NULL, NULL, 0, NULL, NULL);
    fclose(f);
    X509_free(x);
    EVP_PKEY_free(k);
    char err[256];
    if (cb_tls_new(cert, key, &T, err, sizeof err) != CB_OK) { fprintf(stderr, "%s\n", err); abort(); }
    unlink(cert);
    unlink(key);
    rmdir(dir);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 65536) return 0;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
    if (size && write(sv[1], data, size) < 0) {}
    shutdown(sv[1], SHUT_WR);
    cb_tls_conn *c = cb_tls_conn_new(T, sv[0]);
    FZ_CHECK(c != NULL);
    uint8_t buf[512];
    for (int i = 0; i < 64; i++) {
        int want = 0;
        ssize_t r = cb_tls_read(c, buf, sizeof buf, &want);
        FZ_CHECK(r == CB_TLS_WANT || r == CB_TLS_FAIL || (r >= 0 && r <= (ssize_t)sizeof buf));
        FZ_CHECK(r != CB_TLS_WANT || want == CB_TLS_WANT_READ || want == CB_TLS_WANT_WRITE);
        if (r <= 0) break;
    }
    int want = 0;
    ssize_t w = cb_tls_write(c, "x", 1, &want);
    FZ_CHECK(w == 1 || w == 0 || w == CB_TLS_WANT || w == CB_TLS_FAIL);
    cb_tls_conn_free(c);
    close(sv[0]);
    close(sv[1]);
    return 0;
}
