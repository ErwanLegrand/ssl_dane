/*
 *  Author: Viktor Dukhovni
 *  License: THIS CODE IS IN THE PUBLIC DOMAIN.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>

#include <openssl/engine.h>
#include <openssl/conf.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "danessl.h"

static void print_errors(void)
{
    unsigned long err;
    char buffer[1024];
    const char *file;
    const char *data;
    int line;
    int flags;

    while ((err = ERR_get_error_line_data(&file, &line, &data, &flags)) != 0) {
	ERR_error_string_n(err, buffer, sizeof(buffer));
	if (flags & ERR_TXT_STRING)
	    fprintf(stderr, "Error: %s:%s:%d:%s\n", buffer, file, line, data);
	else
	    fprintf(stderr, "Error: %s:%s:%d\n", buffer, file, line);
    }
}

static int add_tlsa(SSL *ssl, const char *argv[])
{
    const EVP_MD *md = 0;
    unsigned char mdbuf[EVP_MAX_MD_SIZE];
    unsigned int mdlen;
    const unsigned char *tlsa_data;
    X509 *cert = 0;
    BIO *bp;
    unsigned char *buf;
    unsigned char *buf2;
    int len;
    uint8_t u = atoi(argv[1]);
    uint8_t s = atoi(argv[2]);
    const char *mdname = *argv[3] ? argv[3] : 0;
    int ret = 0;

    if ((bp = BIO_new_file(argv[4], "r")) == NULL) {
	fprintf(stderr, "error opening %s: %m", argv[4]);
	return 0;
    }
    if (!PEM_read_bio_X509(bp, &cert, 0, 0)) {
	print_errors();
	BIO_free(bp);
	return 0;
    }
    BIO_free(bp);

    /*
     * Extract ASN.1 DER form of certificate or public key.
     */
    switch (s) {
    case DANESSL_SELECTOR_CERT:
	len = i2d_X509(cert, NULL);
	buf2 = buf = (unsigned char *) OPENSSL_malloc(len);
	if (buf)
	    i2d_X509(cert, &buf2);
	break;
    case DANESSL_SELECTOR_SPKI:
	len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), NULL);
	buf2 = buf = (unsigned char *) OPENSSL_malloc(len);
	if (buf)
	    i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &buf2);
	break;
    }
    if (buf == NULL) {
	perror("malloc");
	return 0;
    }
    OPENSSL_assert(buf2 - buf == len);

    if (mdname) {
	if ((md = EVP_get_digestbyname(mdname)) == 0) {
	    fprintf(stderr, "Invalid certificate digest: %s\n", mdname);
	    return 0;
	}
	EVP_Digest(buf, len, mdbuf, &mdlen, md, 0);
	tlsa_data = mdbuf;
	len = mdlen;
    } else {
	tlsa_data = buf;
    }
    ret = DANESSL_add_tlsa(ssl, u, s, mdname, tlsa_data, len);
    OPENSSL_free(buf);
    return ret;
}

static int connect_host_port(const char *host, const char *port)
{
    struct addrinfo *ai = 0;
    struct addrinfo *a;
    struct addrinfo hints;
    int err;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(host, port, &hints, &ai);
    if (err != 0) {
	fprintf(stderr, "getaddrinfo: %s:%s: %s\n",
		host, port, gai_strerror(err));
	exit(EXIT_FAILURE);
    }

    for (a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
	   continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) >= 0) {
	    printf("connected to %s:%s\n", host, port);
	    break;
	}
	fprintf(stderr, "warning: %s:%s", host, port);
	perror("connect");
	(void) close(fd);
	fd = -1;
    }
    freeaddrinfo(ai);
    return fd;
}

static int verify_callback(int ok, X509_STORE_CTX *ctx)
{
    char    buf[8192];
    X509   *cert;
    int     err;
    int     depth;

    cert = X509_STORE_CTX_get_current_cert(ctx);
    err = X509_STORE_CTX_get_error(ctx);
    depth = X509_STORE_CTX_get_error_depth(ctx);

    if (cert)
	X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf));
    else
	strcpy(buf, "<unknown>");
    printf("depth=%d verify=%d err=%d subject=%s\n", depth, ok, err, buf);
    return 1;
}

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s certificate-usage selector matching-type"
	    " certfile \\\n\t\tCAfile service hostname [certname ...]\n",
	    progname);
    fprintf(stderr, "  where, certificate-usage = TLSA certificate usage,\n");
    fprintf(stderr, "\t selector = TLSA selector,\n");
    fprintf(stderr, "\t matching-type = empty string or OpenSSL digest algorithm name,\n");
    fprintf(stderr, "\t PEM certfile provides certificate association data,\n");
    fprintf(stderr, "\t PEM CAfile contains any usage 0/1 trusted roots,\n");
    fprintf(stderr, "\t service = destination port number or service name,\n");
    fprintf(stderr, "\t hostname = destination hostname,\n");
    fprintf(stderr, "\t each certname augments the hostname for name checks.\n");
    exit(1);
}

static void fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "Fatal: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    print_errors();
    exit(1);
}

int main(int argc, const char *argv[])
{
    SSL_CTX *sctx;
    SSL *ssl;
    int fd;

    if (argc < 8)
	usage(argv[0]);

    /* SSL library and DANE library initialization */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_load_error_strings();
    SSL_library_init();
#endif

    if (DANESSL_library_init() <= 0)
	fatal("error initializing DANE library\n");

    /* Initialize context for DANE connections */
    if ((sctx = SSL_CTX_new(SSLv23_client_method())) == 0)
	fatal("error allocating SSL_CTX\n");
    SSL_CTX_set_verify(sctx, SSL_VERIFY_NONE, verify_callback);
    if (*argv[5] && (SSL_CTX_load_verify_locations(sctx, argv[5], 0)) <= 0)
	fatal("error loading CAfile\n");
    if (DANESSL_CTX_init(sctx) <= 0)
	fatal("error initializing SSL_CTX DANE state\n");

    /* Create a connection handle */
    if ((ssl = SSL_new(sctx)) == 0)
	fatal("error allocating SSL handle\n");
    if (DANESSL_init(ssl, argv[7], argv+7) <= 0)
	fatal("error initializing SSL handle DANE state\n");
    if (!add_tlsa(ssl, argv))
	fatal("error adding TLSA RR\n");

    /* Connect to, and verify, a live server */
    if ((fd = connect_host_port(argv[7], argv[6])) >= 0 &&
	SSL_set_fd(ssl, fd) && SSL_connect(ssl)) {
	printf("verify status: %ld\n", SSL_get_verify_result(ssl));
	if (SSL_shutdown(ssl) == 0)
	    SSL_shutdown(ssl);
    }
    print_errors();

    /* Cleanup */
    DANESSL_cleanup(ssl);
    SSL_free(ssl);
    SSL_CTX_free(sctx);

    return 0;
}
