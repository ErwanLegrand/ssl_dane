#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <openssl/opensslv.h>
#include <openssl/err.h>
#include <openssl/crypto.h>

#include "ssl_dane.h"

#define DANE_F_ADD_SKID			100
#define DANE_F_CHECK_END_ENTITY		101
#define DANE_F_GROW_CHAIN		102
#define DANE_F_LIST_ALLOC		103
#define DANE_F_MATCH			104
#define DANE_F_PUSH_EXT			105
#define DANE_F_SET_TRUST_ANCHOR		106
#define DANE_F_SSL_CTX_DANE_INIT	107
#define DANE_F_SSL_DANE_ADD_TLSA	108
#define DANE_F_SSL_DANE_INIT		109
#define DANE_F_SSL_DANE_LIBRARY_INIT	110
#define DANE_F_VERIFY_CERT		111
#define DANE_F_WRAP_CERT		112

#define DANE_R_BAD_CERT			100
#define DANE_R_BAD_CERT_PKEY		101
#define DANE_R_BAD_DATA_LENGTH		102
#define DANE_R_BAD_DIGEST		103
#define DANE_R_BAD_NULL_DATA		104
#define DANE_R_BAD_PKEY			105
#define DANE_R_BAD_SELECTOR		106
#define DANE_R_BAD_USAGE		107
#define DANE_R_DANE_INIT		108
#define DANE_R_DANE_SUPPORT		109
#define DANE_R_LIBRARY_INIT		110
#define DANE_R_NOSIGN_KEY		111
#define DANE_R_SCTX_INIT		112

#ifndef OPENSSL_NO_ERR
#define	DANE_F_PLACEHOLDER		0		/* FIRST! Value TBD */
static ERR_STRING_DATA dane_str_functs[] = {
    {DANE_F_PLACEHOLDER,		"DANE library"},	/* FIRST!!! */
    {DANE_F_ADD_SKID,			"add_skid"},
    {DANE_F_CHECK_END_ENTITY,		"check_end_entity"},
    {DANE_F_GROW_CHAIN,			"grow_chain"},
    {DANE_F_LIST_ALLOC,			"list_alloc"},
    {DANE_F_MATCH,			"match"},
    {DANE_F_PUSH_EXT,			"push_ext"},
    {DANE_F_SET_TRUST_ANCHOR,		"set_trust_anchor"},
    {DANE_F_SSL_CTX_DANE_INIT,		"SSL_CTX_dane_init"},
    {DANE_F_SSL_DANE_ADD_TLSA,		"SSL_dane_add_tlsa"},
    {DANE_F_SSL_DANE_INIT,		"SSL_dane_init"},
    {DANE_F_SSL_DANE_LIBRARY_INIT,	"SSL_dane_library_init"},
    {DANE_F_VERIFY_CERT,		"verify_cert"},
    {DANE_F_WRAP_CERT,			"wrap_cert"},
    {0,					NULL}
};
static ERR_STRING_DATA dane_str_reasons[] = {
    {DANE_R_BAD_CERT,		"Bad TLSA record certificate"},
    {DANE_R_BAD_CERT_PKEY,	"Bad TLSA record certificate public key"},
    {DANE_R_BAD_DATA_LENGTH,	"Bad TLSA record digest length"},
    {DANE_R_BAD_DIGEST,		"Bad TLSA record digest"},
    {DANE_R_BAD_NULL_DATA,	"Bad TLSA record null data"},
    {DANE_R_BAD_PKEY,		"Bad TLSA record public key"},
    {DANE_R_BAD_SELECTOR,	"Bad TLSA record selector"},
    {DANE_R_BAD_USAGE,		"Bad TLSA record usage"},
    {DANE_R_DANE_INIT,		"SSL_dane_init() required"},
    {DANE_R_DANE_SUPPORT,	"DANE library features not supported"},
    {DANE_R_LIBRARY_INIT,	"SSL_dane_library_init() required"},
    {DANE_R_SCTX_INIT,		"SSL_CTX_dane_init() required"},
    {DANE_R_NOSIGN_KEY,		"Certificate usage 2 requires EC support"},
    {0,				NULL}
};
#endif

#define DANEerr(f, r) ERR_PUT_error(err_lib_dane, (f), (r), __FILE__, __LINE__)

static int err_lib_dane = -1;

static void load_dane_strings(void) {
#ifndef OPENSSL_NO_ERR
    if (err_lib_dane > 0) {
	if (ERR_func_error_string(dane_str_functs[0].error))
	    return;
	ERR_load_strings(err_lib_dane, dane_str_functs);
	ERR_load_strings(err_lib_dane, dane_str_reasons);
    }
#endif
}

static int safe_init(
	volatile int *value,
	int (*init)(void),
	void (*postinit)(void)
)
{
    int wlock = 0;

    CRYPTO_r_lock(CRYPTO_LOCK_SSL_CTX);
    if (*value < 0) {
	CRYPTO_r_unlock(CRYPTO_LOCK_SSL_CTX);
	CRYPTO_w_lock(CRYPTO_LOCK_SSL_CTX);
	wlock = 1;
	if (*value < 0) {
	    *value = init();
	    if (postinit)
		postinit();
	}
    }
    if (wlock)
	CRYPTO_w_unlock(CRYPTO_LOCK_SSL_CTX);
    else
	CRYPTO_r_unlock(CRYPTO_LOCK_SSL_CTX);
    return *value;
}

#undef SIGN_SUPPORT
#if OPENSSL_VERSION_NUMBER >= 0x1000000fL && \
	(defined(X509_V_FLAG_PARTIAL_CHAIN) || !defined(OPENSSL_NO_ECDH))
#define SIGN_SUPPORT
#endif

#include <openssl/safestack.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>

static void (*cert_free)(void *) = (void (*)(void *)) X509_free;
static void (*pkey_free)(void *) = (void (*)(void *)) EVP_PKEY_free;

typedef struct dane_list {
    struct dane_list *next;
    void *value;
} *dane_list;

#define LINSERT(h, e) do { (e)->next = (h); (h) = (e); } while (0)

typedef struct DANE_HOST_LIST {
    struct DANE_HOST_LIST *next;
    char *value;
} *DANE_HOST_LIST;

typedef struct dane_data {
    size_t datalen;
    unsigned char data[0];
} *dane_data;

typedef struct DANE_DATA_LIST {
    struct DANE_DATA_LIST *next;
    dane_data value;
} *DANE_DATA_LIST;

typedef struct dane_mtype {
    int mdlen;
    const EVP_MD *md;
    DANE_DATA_LIST data;
} *dane_mtype;

typedef struct DANE_MTYPE_LIST {
    struct DANE_MTYPE_LIST *next;
    dane_mtype value;
} *DANE_MTYPE_LIST;

typedef struct dane_selector {
    uint8_t selector;
    DANE_MTYPE_LIST mtype;
} *dane_selector;

typedef struct DANE_SELECTOR_LIST {
    struct DANE_SELECTOR_LIST *next;
    dane_selector value;
} *DANE_SELECTOR_LIST;

typedef struct DANE_PKEY_LIST {
    struct DANE_PKEY_LIST *next;
    EVP_PKEY *value;
} *DANE_PKEY_LIST;

typedef struct DANE_CERT_LIST {
    struct DANE_CERT_LIST *next;
    X509 *value;
} *DANE_CERT_LIST;

typedef struct SSL_DANE {
    int            (*verify)(X509_STORE_CTX *);
    STACK_OF(X509) *roots;
    STACK_OF(X509) *chain;
    const char     *thost;		/* TLSA base domain */
    char	   *mhost;		/* Matched, peer name */
    DANE_PKEY_LIST pkeys;
    DANE_CERT_LIST certs;
    DANE_HOST_LIST hosts;
    DANE_SELECTOR_LIST selectors[SSL_DANE_USAGE_LAST + 1];
    int            depth;
    int		   multi;		/* Multi-label wildcards? */
} SSL_DANE;

#ifndef X509_V_ERR_HOSTNAME_MISMATCH
#define X509_V_ERR_HOSTNAME_MISMATCH X509_V_ERR_APPLICATION_VERIFICATION
#endif

static int dane_idx = -1;
static EVP_PKEY *signkey;
static const EVP_MD *signmd;

#undef WRAP_SIGNED
#ifdef X509_V_FLAG_PARTIAL_CHAIN	/* OpenSSL >= 1.0.2 */
static int wrap_signed = 0;
#else
#define WRAP_SIGNED
static int wrap_signed = 1;
#endif

#if defined(WRAP_SIGNED) && defined(SIGN_SUPPORT)
static EVP_PKEY *gensignkey(void)
{
    EVP_PKEY *key = 0;

    EC_KEY *eckey;
    EC_GROUP *group = 0;

    ERR_clear_error();

    if ((eckey = EC_KEY_new()) != 0
	&& (group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)) != 0
	&& (EC_GROUP_set_asn1_flag(group, OPENSSL_EC_NAMED_CURVE),
	    EC_KEY_set_group(eckey, group))
	&& EC_KEY_generate_key(eckey)
	&& (key = EVP_PKEY_new()) != 0
	&& !EVP_PKEY_set1_EC_KEY(key, eckey)) {
	EVP_PKEY_free(key);
	key = 0;
    }
    if (group)
	EC_GROUP_free(group);
    if (eckey)
	EC_KEY_free(eckey);
    return (key);
}
#endif

static void dane_init(void)
{
#if defined(WRAP_SIGNED) && defined(SIGN_SUPPORT)
    /*
     * Initialize the signature algorithm and signing key for releases in
     * which trust-anchors must be self-signed and thus intermediate
     * trust-anchors need to be dynamically signed by an internal root CA.
     */
    if (signmd == 0)
	signmd = EVP_get_digestbyname(LN_sha256);
    if (signkey == 0)
	signkey = gensignkey();
#endif

    /*
     * Store library id in zeroth function slot, used to locate the library
     * name.  This must be done before we load the error strings.
     */
    dane_str_functs[0].error |= ERR_PACK(err_lib_dane, 0, 0);
    load_dane_strings();

    /*
     * Register an SSL index for the connection-specific SSL_DANE structure.
     * Using a separate index makes it possible to add DANE support to
     * existing OpenSSL releases that don't have a suitable pointer in the
     * SSL structure.
     */
    dane_idx = SSL_get_ex_new_index(0, 0, 0, 0, 0);
}

static int match(
	DANE_SELECTOR_LIST slist,
	X509 *cert,
	int depth
)
{
    int matched;

    /*
     * Note, set_trust_anchor() needs to know whether the match was for a
     * pkey digest or a certificate digest.  We return MATCHED_PKEY or
     * MATCHED_CERT accordingly.
     */
#define MATCHED_CERT (SSL_DANE_SELECTOR_CERT + 1)
#define MATCHED_PKEY (SSL_DANE_SELECTOR_SPKI + 1)

    /*
     * Loop over each selector, mtype, and associated data element looking
     * for a match.
     */
    for (matched = 0; !matched && slist; slist = slist->next) {
	DANE_MTYPE_LIST m;
	unsigned char mdbuf[EVP_MAX_MD_SIZE];
	unsigned char *buf;
	unsigned char *buf2;
	unsigned int len;

	/*
	 * Extract ASN.1 DER form of certificate or public key.
	 */
	switch (slist->value->selector) {
	case SSL_DANE_SELECTOR_CERT:
	    len = i2d_X509(cert, NULL);
	    buf2 = buf = (unsigned char *) OPENSSL_malloc(len);
	    if (buf)
		i2d_X509(cert, &buf2);
	    break;
	case SSL_DANE_SELECTOR_SPKI:
	    len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), NULL);
	    buf2 = buf = (unsigned char *) OPENSSL_malloc(len);
	    if (buf)
		i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &buf2);
	    break;
	}

	if (buf == NULL) {
	    DANEerr(DANE_F_MATCH, ERR_R_MALLOC_FAILURE);
	    return 0;
	}
	OPENSSL_assert(buf2 - buf == len);

	/*
	 * Loop over each mtype and data element
	 */
	for (m = slist->value->mtype; !matched && m; m = m->next) {
	    DANE_DATA_LIST d;
	    unsigned char *cmpbuf = buf;
	    unsigned int cmplen = len;

	    /*
	     * If it is a digest, compute the corresponding digest of the
	     * DER data for comparison, otherwise, use the full object.
	     */
	    if (m->value->md) {
		cmpbuf = mdbuf;
		if (!EVP_Digest(buf, len, cmpbuf, &cmplen, m->value->md, 0))
		    matched = -1;
	    }
	    for (d = m->value->data; !matched && d; d = d->next)
		if (cmplen == d->value->datalen &&
		    memcmp(cmpbuf, d->value->data, cmplen) == 0)
		    matched = slist->value->selector + 1;
	}

	OPENSSL_free(buf);
    }

    return matched;
}

static int push_ext(X509 *cert, X509_EXTENSION *ext)
{
    X509_EXTENSIONS *exts;

    if ((exts = cert->cert_info->extensions) == 0)
	exts = cert->cert_info->extensions = sk_X509_EXTENSION_new_null();
    if (!exts) {
	DANEerr(DANE_F_PUSH_EXT, ERR_R_MALLOC_FAILURE);
	return 0;
    }
    return sk_X509_EXTENSION_push(exts, ext);
}

static int add_ext(
	X509 *issuer,
	X509 *subject,
	int ext_nid,
	char *ext_val
)
{
    X509V3_CTX v3ctx;
    X509_EXTENSION *ext;

    X509V3_set_ctx(&v3ctx, issuer, subject, 0, 0, 0);
    if ((ext = X509V3_EXT_conf_nid(0, &v3ctx, ext_nid, ext_val)) == 0)
	return 0;
    if (push_ext(subject, ext))
	return 1;
    X509_EXTENSION_free(ext);
    return 0;
}

static int set_serial(
	X509 *cert,
	AUTHORITY_KEYID *akid,
	X509 *subject
)
{
    int ret = 0;
    BIGNUM *bn;

    if (akid && akid->serial)
	return (X509_set_serialNumber(cert, akid->serial));

    /*
     * Add one to subject's serial to avoid collisions between TA serial and
     * serial of signing root.
     */
    if ((bn = ASN1_INTEGER_to_BN(X509_get_serialNumber(subject), 0)) != 0
	&& BN_add_word(bn, 1)
	&& BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert)))
	ret = 1;

    if (bn)
	BN_free(bn);
    return ret;
}

static int add_akid(
	X509 *cert,
	AUTHORITY_KEYID *akid
)
{
    ASN1_STRING *id;
    unsigned char c = 0;
    int ret = 0;

    /*
     * 0 will never be our subject keyid from a SHA-1 hash, but it could be
     * our subject keyid if forced from child's akid.  If so, set our
     * authority keyid to 1.  This way we are never self-signed, and thus
     * exempt from any potential (off by default for now in OpenSSL)
     * self-signature checks!
     */
    id = (ASN1_STRING *) ((akid && akid->keyid) ? akid->keyid : 0);
    if (id && M_ASN1_STRING_length(id) == 1 && *M_ASN1_STRING_data(id) == c)
	c = 1;

    if ((akid = AUTHORITY_KEYID_new()) != 0
	&& (akid->keyid = ASN1_OCTET_STRING_new()) != 0
	&& M_ASN1_OCTET_STRING_set(akid->keyid, (void *) &c, 1)
	&& X509_add1_ext_i2d(cert, NID_authority_key_identifier, akid, 0, 0))
	ret = 1;
    if (akid)
	AUTHORITY_KEYID_free(akid);
    return ret;
}

static int add_skid(
	X509 *cert,
	AUTHORITY_KEYID *akid
)
{
    int ret = 0;
    int nid = NID_subject_key_identifier;
    X509_EXTENSION *ext;

    if (akid && akid->keyid) {
	ASN1_STRING *id = (ASN1_STRING *) (akid->keyid);
	ext = X509_EXTENSION_create_by_NID(0, nid, 0, id);
	if (ext) {
	    ret = push_ext(cert, ext);
	} else
	    DANEerr(DANE_F_ADD_SKID, ERR_R_MALLOC_FAILURE);
    } else {
	ret = add_ext(0, cert, NID_subject_key_identifier, "hash");
    }
    return ret;
}

static X509_NAME *akid_issuer_name(
	AUTHORITY_KEYID *akid
)
{
    if (akid && akid->issuer) {
	int     i;
	GENERAL_NAMES *gens = akid->issuer;

	for (i = 0; i < sk_GENERAL_NAME_num(gens); ++i) {
	    GENERAL_NAME *gn = sk_GENERAL_NAME_value(gens, i);

	    if (gn->type == GEN_DIRNAME)
		return (gn->d.dirn);
	}
    }
    return 0;
}

static int set_issuer_name(
	X509 *cert,
	AUTHORITY_KEYID *akid
)
{
    X509_NAME *name = akid_issuer_name(akid);

    /*
     * If subject's akid specifies an authority key identifer issuer name, we
     * must use that.
     */
    if (name)
	return X509_set_issuer_name(cert, name);
    return X509_set_issuer_name(cert, X509_get_subject_name(cert));
}

static int grow_chain(STACK_OF(X509) **skptr, X509 *cert, int add_trust)
{
    static ASN1_OBJECT *trust = 0;

    if (add_trust && trust == 0 &&
       (trust = OBJ_nid2obj(NID_server_auth)) == 0) {
	DANEerr(DANE_F_GROW_CHAIN, ERR_R_MALLOC_FAILURE);
	return 0;
    }
    if (!*skptr && (*skptr = sk_X509_new_null()) == 0) {
	DANEerr(DANE_F_GROW_CHAIN, ERR_R_MALLOC_FAILURE);
	return 0;
    }

    if (cert) {
	if (add_trust && !X509_add1_trust_object(cert, trust))
	    return 0;
	CRYPTO_add(&cert->references, 1, CRYPTO_LOCK_X509);
	if (!sk_X509_push(*skptr, cert)) {
	    X509_free(cert);
	    DANEerr(DANE_F_GROW_CHAIN, ERR_R_MALLOC_FAILURE);
	    return 0;
	}
    }
    return 1;
}

static int wrap_key(
	SSL_DANE *dane,
	int depth,
	EVP_PKEY *key,
	X509 *subject
)
{
    int ret = 1;
    int selfsigned = 0;
    X509 *cert = 0;
    AUTHORITY_KEYID *akid;
    X509_NAME *name = X509_get_issuer_name(subject);
    X509_NAME *akid_name;

    /*
     * Record the depth of the trust-anchor certificate.
     */
    if (dane->depth < 0)
	dane->depth = depth + 1;

    /*
     * If key is NULL generate a self-signed root CA, with key "signkey",
     * otherwise an intermediate CA signed by above.
     */
    if ((cert = X509_new()) == 0)
	return 0;

    /*
     * XXX: Uncaught error condition:
     *
     * The return value is NULL both when the extension is missing, and when
     * OpenSSL rans out of memory while parsing the extension.
     */
    ERR_clear_error();
    akid = X509_get_ext_d2i(subject, NID_authority_key_identifier, 0, 0);
    /* XXX: Should we peek at the error stack here??? */
    if ((akid_name = akid_issuer_name(akid)) == 0
	|| X509_NAME_cmp(name, akid_name) == 0)
	selfsigned = 1;

    /* CA cert valid for +/- 30 days */
    if (!X509_set_version(cert, 2)
	|| !set_serial(cert, akid, subject)
	|| !X509_set_subject_name(cert, name)
	|| !set_issuer_name(cert, akid)
	|| !X509_gmtime_adj(X509_get_notBefore(cert), -30 * 86400L)
	|| !X509_gmtime_adj(X509_get_notAfter(cert), 30 * 86400L)
	|| !X509_set_pubkey(cert, key ? key : signkey)
	|| !add_ext(0, cert, NID_basic_constraints, "CA:TRUE")
	|| (key && !selfsigned && !add_akid(cert, akid))
	|| !add_skid(cert, akid)
	|| (wrap_signed
	    && (!X509_sign(cert, signkey, signmd)
		|| (key && !selfsigned
		    && !wrap_key(dane, depth + 1, 0, cert))))) {
	ret = 0;
    }
    if (akid)
	AUTHORITY_KEYID_free(akid);
    if (ret) {
	if (key && !selfsigned && wrap_signed)
	    ret = grow_chain(&dane->chain, cert, 0);
	else
	    ret = grow_chain(&dane->roots, cert, 1);
    }
    if (cert)
	X509_free(cert);
    return ret;
}

static int wrap_cert(
	SSL_DANE *dane,
	int depth,
	X509 *tacert,
	X509 *subject
)
{
    int ret = 1;
    X509 *cert;
    int len;
    unsigned char *asn1;
    unsigned char *buf;

    dane->depth = depth;

    /*
     * If the TA certificate is self-issued, use it directly.
     */
    if (!wrap_signed
	|| X509_check_issued(tacert, tacert) == X509_V_OK)
	return grow_chain(&dane->roots, tacert, 1);

    /* Deep-copy tacert by converting to ASN.1 and back */
    len = i2d_X509(tacert, NULL);
    asn1 = buf = (unsigned char *) OPENSSL_malloc(len);
    if (!buf) {
	DANEerr(DANE_F_WRAP_CERT, ERR_R_MALLOC_FAILURE);
	return 0;
    }
    i2d_X509(tacert, &buf);
    OPENSSL_assert(buf - asn1 == len);

    buf = asn1;
    cert = d2i_X509(0, (unsigned const char **) &buf, len);
    if (!cert) {
	OPENSSL_free(asn1);
	return 0;
    }
    OPENSSL_assert(buf - asn1 == len);
    OPENSSL_free(asn1);

    if (!grow_chain(&dane->chain, cert, 0)) {
	X509_free(cert);
	return 0;
    }

    /* Sign and wrap TA cert with internal "signkey" */
    if (!X509_sign(cert, signkey, signmd)
	|| !wrap_key(dane, depth + 1, signkey, cert))
	ret = 0;
    X509_free(cert);
    return ret;
}

static int ta_signed(
	SSL_DANE *dane,
	X509 *cert,
	int depth
)
{
    DANE_CERT_LIST x;
    DANE_PKEY_LIST k;
    EVP_PKEY *pk;
    int done = 0;

    /*
     * First check whether issued and signed by a TA cert, this is cheaper
     * than the bare-public key checks below, since we can determine whether
     * the candidate TA certificate issued the certificate to be checked
     * first (name comparisons), before we bother with signature checks
     * (public key operations).
     */
    for (x = dane->certs; !done && x; x = x->next) {
	if (X509_check_issued(x->value, cert) == X509_V_OK) {
	    if ((pk = X509_get_pubkey(x->value)) == 0) {
		/*
		 * The cert originally contained a valid pkey, which does
		 * not just vanish, so this is most likely a memory error.
		 */
		done = -1;
		break;
	    }
	    /* Check signature, since some other TA may work if not this. */
	    if (X509_verify(cert, pk) > 0)
		done = wrap_cert(dane, depth + 1, x->value, cert) ? 1 : -1;
	    EVP_PKEY_free(pk);
	}
    }

    /*
     * With bare TA public keys, we can't check whether the trust chain is
     * issued by the key, but we can determine whether it is signed by the
     * key, so we go with that.
     *
     * Ideally, the corresponding certificate was presented in the chain, and we
     * matched it by its public key digest one level up.  This code is here
     * to handle adverse conditions imposed by sloppy administrators of
     * receiving systems with poorly constructed chains.
     *
     * We'd like to optimize out keys that should not match when the cert's
     * authority key id does not match the key id of this key computed via
     * the RFC keyid algorithm (SHA-1 digest of public key bit-string sans
     * ASN1 tag and length thus also excluding the unused bits field that is
     * logically part of the length).  However, some CAs have a non-standard
     * authority keyid, so we lose.  Too bad.
     */
    for (k = dane->pkeys; !done && k; k = k->next)
	if (X509_verify(cert, k->value) > 0)
	    done = wrap_key(dane, depth, k->value, cert) ? 1 : -1;

    return done;
}

static int set_trust_anchor(
	X509_STORE_CTX *ctx,
	SSL_DANE *dane,
	X509 *cert
)
{
    int matched = 0;
    int n;
    int i;
    int depth = 0;
    EVP_PKEY *takey;
    X509 *ca;
    STACK_OF(X509) *in = ctx->untrusted;	/* XXX: Accessor? */

    if (!grow_chain(&dane->chain, 0, 0))
	return -1;

    /* Accept a degenerate case: depth 0 self-signed trust-anchor. */
    if (X509_check_issued(cert, cert) == X509_V_OK) {
	matched = match(dane->selectors[SSL_DANE_USAGE_TRUSTED_CA], cert, 0);
	if (matched > 0 && !grow_chain(&dane->roots, cert, 1))
	    matched = -1;
	return matched;
    }

    /* Make a shallow copy of the input untrusted chain. */
    if ((in = sk_X509_dup(in)) == 0) {
	DANEerr(DANE_F_SET_TRUST_ANCHOR, ERR_R_MALLOC_FAILURE);
	return -1;
    }

    /*
     * At each iteration we consume the issuer of the current cert.  This
     * reduces the length of the "in" chain by one.  If no issuer is found,
     * we are done.  We also stop when a certificate matches a TA in the
     * peer's TLSA RRset.
     *
     * Caller ensures that the initial certificate is not self-signed.
     */
    for (n = sk_X509_num(in); n > 0; --n, ++depth) {
	for (i = 0; i < n; ++i)
	    if (X509_check_issued(sk_X509_value(in, i), cert) == X509_V_OK)
		break;

	/*
	 * Final untrusted element with no issuer in the peer's chain, it may
	 * however be signed by a pkey or cert obtained via a TLSA RR.
	 */
	if (i == n)
	    break;

	/* Peer's chain contains an issuer ca. */
	ca = sk_X509_delete(in, i);

	/* If not a trust anchor, record untrusted ca and continue. */
	if ((matched = match(dane->selectors[SSL_DANE_USAGE_TRUSTED_CA], ca,
			     depth + 1)) == 0) {
	    if (grow_chain(&dane->chain, ca, 0)) {
		if (!X509_check_issued(ca, ca) == X509_V_OK) {
		    /* Restart with issuer as subject */
		    cert = ca;
		    continue;
		}
		/* Final self-signed element, skip ta_signed() check. */
		cert = 0;
	    } else
		matched = -1;
	} else if (matched == MATCHED_CERT) {
	    if (!wrap_cert(dane, depth, ca, cert))
		matched = -1;
	} else if (matched == MATCHED_PKEY) {
	    if ((takey = X509_get_pubkey(ca)) == 0 ||
		!wrap_key(dane, depth, takey, cert)) {
		if (takey)
		    EVP_PKEY_free(takey);
		else
		    DANEerr(DANE_F_SET_TRUST_ANCHOR, ERR_R_MALLOC_FAILURE);
		matched = -1;
	    }
	}
	break;
    }

    /* Shallow free the duplicated input untrusted chain. */
    sk_X509_free(in);

    /*
     * When the loop exits, if "cert" is set, it is not self-signed and has
     * no issuer in the chain, we check for a possible signature via a DNS
     * obtained TA cert or public key.
     */
    if (matched < 0 ||
	(matched == 0 &&
	 (!cert || (matched = ta_signed(dane, cert, depth)) <= 0)))
	return matched;

    /*
     * Check that setting the untrusted chain updates the expected
     * structure member at the expected offset.
     */
    X509_STORE_CTX_trusted_stack(ctx, dane->roots);
    X509_STORE_CTX_set_chain(ctx, dane->chain);
    OPENSSL_assert(ctx->untrusted == dane->chain);

    return matched;
}

static int check_end_entity(
    X509_STORE_CTX *ctx,
    SSL_DANE *dane,
    X509 *cert
)
{
    int matched;

    matched = match(dane->selectors[SSL_DANE_USAGE_FIXED_LEAF], cert, 0);
    if (matched > 0) {
	if (ctx->chain == 0) {
	    if ((ctx->chain = sk_X509_new_null()) != 0 &&
		sk_X509_push(ctx->chain, cert)) {
		CRYPTO_add(&cert->references, 1, CRYPTO_LOCK_X509);
	    } else {
		DANEerr(DANE_F_CHECK_END_ENTITY, ERR_R_MALLOC_FAILURE);
		return -1;
	    }
	}
    }
    return matched;
}

static int match_name(const char *certid, SSL_DANE *dane)
{
    int multi = dane->multi;
    DANE_HOST_LIST hosts = dane->hosts;

    for (/* NOP */; hosts; hosts = hosts->next) {
	int match_subdomain = 0;
	const char *domain = hosts->value;
	const char *parent;
	int idlen;
	int domlen;

	if (*domain == '.' && domain[1] != '\0') {
	    ++domain;
	    match_subdomain = 1;
	}

	/*
	 * Sub-domain match: certid is any sub-domain of hostname.
	 */
	if (match_subdomain) {
	    if ((idlen = strlen(certid)) > (domlen = strlen(domain)) + 1
		&& certid[idlen - domlen - 1] == '.'
		&& !strcasecmp(certid + (idlen - domlen), domain))
		return 1;
	    else
		continue;
	}

	/*
	 * Exact match and initial "*" match. The initial "*" in a certid
	 * matches one (if multi is false) or more hostname components under
	 * the condition that the certid contains multiple hostname components.
	 */
	if (!strcasecmp(certid, domain)
	    || (certid[0] == '*' && certid[1] == '.' && certid[2] != 0
		&& (parent = strchr(domain, '.')) != 0
		&& (idlen = strlen(certid + 1)) <= (domlen = strlen(parent))
		&& strcasecmp(multi ? parent + domlen - idlen : parent,
			      certid + 1) == 0))
	    return 1;
    }
    return 0;
}

static char *check_name(char *name, int len)
{
    register char *cp = name + len;

    while (len > 0 && *--cp == 0)
	--len;				/* Ignore trailing NULs */
    if (len <= 0)
	return 0;
    for (cp = name; *cp; cp++) {
	register char c = *cp;
	if (!((c >= 'a' && c <= 'z') ||
	      (c >= '0' && c <= '9') ||
	      (c >= 'A' && c <= 'Z') ||
	      (c == '.' || c == '-') ||
	      (c == '*')))
	    return 0;			/* Only LDH, '.' and '*' */
    }
    if (cp - name != len)		/* Guard against internal NULs */
	return 0;
    return name;
}

static char *parse_dns_name(const GENERAL_NAME *gn)
{
    if (gn->type != GEN_DNS)
	return 0;
    if (ASN1_STRING_type(gn->d.ia5) != V_ASN1_IA5STRING)
	return 0;
    return check_name((char *) ASN1_STRING_data(gn->d.ia5),
		      ASN1_STRING_length(gn->d.ia5));
}

static char *parse_subject_name(X509 *cert)
{
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_ENTRY *entry;
    ASN1_STRING *entry_str;
    unsigned char *namebuf;
    int nid = NID_commonName;
    int len;
    int i;

    if (name == 0 || (i = X509_NAME_get_index_by_NID(name, nid, -1)) < 0)
	return 0;
    if ((entry = X509_NAME_get_entry(name, i)) == 0)
	return 0;
    if ((entry_str = X509_NAME_ENTRY_get_data(entry)) == 0)
	return 0;

    if ((len = ASN1_STRING_to_UTF8(&namebuf, entry_str)) < 0)
	return 0;
    if (len <= 0 || check_name((char *) namebuf, len) == 0) {
	OPENSSL_free(namebuf);
	return 0;
    }
    return (char *) namebuf;
}

static int name_check(SSL_DANE *dane, X509 *cert)
{
    int matched = 0;
    int got_altname = 0;
    GENERAL_NAMES *gens;

    gens = X509_get_ext_d2i(cert, NID_subject_alt_name, 0, 0);
    if (gens) {
	int n = sk_GENERAL_NAME_num(gens);
	int i;

	for (i = 0; i < n; ++i) {
	    const GENERAL_NAME *gn = sk_GENERAL_NAME_value(gens, i);
	    const char *certid;

	    if (gn->type != GEN_DNS)
		continue;
	    got_altname = 1;
	    certid = parse_dns_name(gn);
	    if (certid && *certid) {
		if ((matched = match_name(certid, dane)) == 0)
		    continue;
		if ((dane->mhost = OPENSSL_strdup(certid)) == 0)
		    matched = -1;
		break;
	    }
	}
	GENERAL_NAMES_free(gens);
    }

    /*
     * XXX: Should the subjectName be skipped when *any* altnames are present,
     * or only when DNS altnames are present?
     */
    if (got_altname == 0) {
	char *certid = parse_subject_name(cert);
	if (certid != 0 && *certid && (matched = match_name(certid, dane)) != 0)
	    dane->mhost = certid;	/* Already a copy */
    }
    return matched;
}

static int verify_chain(X509_STORE_CTX *ctx)
{
    int (*cb)(int, X509_STORE_CTX *) = ctx->verify_cb;
    int ssl_idx = SSL_get_ex_data_X509_STORE_CTX_idx();
    SSL *ssl = X509_STORE_CTX_get_ex_data(ctx, ssl_idx);
    SSL_DANE *dane = SSL_get_ex_data(ssl, dane_idx);
    X509 *cert = ctx->cert;		/* XXX: accessor? */
    int matched = 0;

    /*
     * Satisfy at least one usage 0 or 1 constraint, unless we've already
     * matched a usage 2 trust anchor.
     */
    if (!dane->roots) {
	DANE_SELECTOR_LIST issuer_rrs;
	DANE_SELECTOR_LIST leaf_rrs;
	int chain_length = sk_X509_num(ctx->chain);
	int n;

	issuer_rrs = dane->selectors[SSL_DANE_USAGE_LIMIT_ISSUER];
	leaf_rrs = dane->selectors[SSL_DANE_USAGE_LIMIT_LEAF];
	n = issuer_rrs ? chain_length : 1;

	while (!matched && --n >= 0) {
	    X509 *cert = sk_X509_value(ctx->chain, n);

	    if (issuer_rrs)
		matched = match(issuer_rrs, cert, n);
	    if (!matched && n == 0 && leaf_rrs)
		matched = match(leaf_rrs, cert, 0);
	}
	if (matched < 0)
	    return 0;

	if (!matched) {
	    ctx->error_depth = chain_length - 1;
	    X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_UNTRUSTED);
	    if (!cb(0, ctx))
		return 0;
	}
    }

    if ((matched = name_check(dane, cert)) < 0)
	return 0;

    if (!matched) {
	ctx->error_depth = 0;
	X509_STORE_CTX_set_error(ctx, X509_V_ERR_HOSTNAME_MISMATCH);
	if (!cb(0, ctx))
	    return 0;
    }

    return dane->verify(ctx);
}

static int verify_cert(X509_STORE_CTX *ctx, void *unused_ctx)
{
    static int ssl_idx = -1;
    SSL *ssl;
    SSL_DANE *dane;
    int (*cb)(int, X509_STORE_CTX *) = ctx->verify_cb;
    int matched;
    X509 *cert = ctx->cert;		/* XXX: accessor? */

    if (ssl_idx < 0)
	ssl_idx = SSL_get_ex_data_X509_STORE_CTX_idx();
    if (dane_idx < 0) {
	DANEerr(DANE_F_VERIFY_CERT, ERR_R_MALLOC_FAILURE);
	return -1;
    }

    ssl = X509_STORE_CTX_get_ex_data(ctx, ssl_idx);
    if ((dane = SSL_get_ex_data(ssl, dane_idx)) == 0 || cert == 0)
	return X509_verify_cert(ctx);

    if (dane->selectors[SSL_DANE_USAGE_FIXED_LEAF]) {
	if ((matched = check_end_entity(ctx, dane, cert)) > 0) {
	    ctx->error_depth = 0;
	    return cb(1, ctx);
	}
	if (matched < 0)
	    return -1;
    }

    if (dane->selectors[SSL_DANE_USAGE_TRUSTED_CA] &&
	set_trust_anchor(ctx, dane, cert) < 0)
	return -1;

    /*
     * Name checks and usage 0/1 constraint enforcement are delayed until
     * X509_verify_cert() builds the full chain and calls our verify_chain()
     * wrapper.
     */
    dane->verify = ctx->verify;
    ctx->verify = verify_chain;

    return X509_verify_cert(ctx);
}

static dane_list list_alloc(size_t vsize)
{
    void *value = (void *) OPENSSL_malloc(vsize);
    dane_list l;

    if (value == 0) {
	DANEerr(DANE_F_LIST_ALLOC, ERR_R_MALLOC_FAILURE);
	return 0;
    }
    if ((l = (dane_list) OPENSSL_malloc(sizeof(*l))) == 0) {
	OPENSSL_free(value);
	DANEerr(DANE_F_LIST_ALLOC, ERR_R_MALLOC_FAILURE);
	return 0;
    }
    l->next = 0;
    l->value = value;
    return l;
}

static void list_free(void *list, void (*f)(void *))
{
    dane_list head = (dane_list) list;
    dane_list next;

    for (/* NOP */; head; head = next) {
	next = head->next;
	if (f && head->value)
	    f(head->value);
	OPENSSL_free(head);
    }
}

static void dane_mtype_free(void *p)
{
    list_free(((dane_mtype) p)->data, OPENSSL_freeFunc);
    OPENSSL_free(p);
}

static void dane_selector_free(void *p)
{
    list_free(((dane_selector) p)->mtype, dane_mtype_free);
    OPENSSL_free(p);
}

void SSL_dane_cleanup(SSL *ssl)
{
    SSL_DANE *dane;
    int u;

    if (dane_idx < 0 || (dane = SSL_get_ex_data(ssl, dane_idx)) == 0)
	return;
    (void) SSL_set_ex_data(ssl, dane_idx, 0);

    if (dane->hosts)
	list_free(dane->hosts, OPENSSL_freeFunc);
    if (dane->mhost)
	OPENSSL_free(dane->mhost);
    for (u = 0; u <= SSL_DANE_USAGE_LAST; ++u)
	if (dane->selectors[u])
	    list_free(dane->selectors[u], dane_selector_free);
    if (dane->pkeys)
	list_free(dane->pkeys, pkey_free);
    if (dane->certs)
	list_free(dane->certs, cert_free);
    if (dane->roots)
	sk_X509_pop_free(dane->roots, X509_free);
    if (dane->chain)
	sk_X509_pop_free(dane->chain, X509_free);
    OPENSSL_free(dane);
}

static DANE_HOST_LIST host_list_init(const char **src)
{
    DANE_HOST_LIST head = 0;

    while (*src) {
	DANE_HOST_LIST elem = (DANE_HOST_LIST) OPENSSL_malloc(sizeof(*elem));
	if (elem == 0) {
	    list_free(head, OPENSSL_freeFunc);
	    return 0;
	}
	elem->value = OPENSSL_strdup(*src++);
	LINSERT(head, elem);
    }
    return head;
}

int SSL_dane_add_tlsa(
	SSL *ssl,
	uint8_t usage,
	uint8_t selector,
	const char *mdname,
	unsigned const char *data,
	size_t dlen
)
{
    SSL_DANE *dane;
    DANE_SELECTOR_LIST s = 0;
    DANE_MTYPE_LIST m = 0;
    DANE_DATA_LIST d = 0;
    DANE_CERT_LIST xlist = 0;
    DANE_PKEY_LIST klist = 0;
    const EVP_MD *md = 0;

    if (dane_idx < 0 || (dane = SSL_get_ex_data(ssl, dane_idx)) == 0) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_DANE_INIT);
	return -1;
    }

    if (usage > SSL_DANE_USAGE_LAST) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_USAGE);
	return 0;
    }
    if (selector > SSL_DANE_SELECTOR_LAST) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_SELECTOR);
	return 0;
    }
    if (mdname && (md = EVP_get_digestbyname(mdname)) == 0) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_DIGEST);
	return 0;
    }
    if (!data) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_NULL_DATA);
	return 0;
    }
    if (mdname && dlen != EVP_MD_size(md)) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_DATA_LENGTH);
	return 0;
    }

    if (usage == SSL_DANE_USAGE_TRUSTED_CA && wrap_signed && !signkey) {
	DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_NOSIGN_KEY);
	return 0;
    }

    if (mdname == 0) {
	X509 *x = 0;
	EVP_PKEY *k = 0;
	const unsigned char *p = data;

#define xklistinit(lvar, ltype, var, freeFunc) do { \
	    (lvar) = (ltype) OPENSSL_malloc(sizeof(*(lvar))); \
	    if ((lvar) == 0) { \
		DANEerr(DANE_F_SSL_DANE_ADD_TLSA, ERR_R_MALLOC_FAILURE); \
		freeFunc((var)); \
		return 0; \
	    } \
	    (lvar)->next = 0; \
	    lvar->value = var; \
	} while (0)
#define xkfreeret(ret) do { \
	    if (xlist) list_free(xlist, cert_free); \
	    if (klist) list_free(klist, pkey_free); \
	    return (ret); \
	} while (0)

	switch (selector) {
	case SSL_DANE_SELECTOR_CERT:
	    if (!d2i_X509(&x, &p, dlen) || dlen != p - data) {
		if (x)
		    X509_free(x);
		DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_CERT);
		return 0;
	    }
	    k = X509_get_pubkey(x);
	    EVP_PKEY_free(k);
	    if (k == 0) {
		X509_free(x);
		DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_CERT_PKEY);
		return 0;
	    }
	    if (usage == SSL_DANE_USAGE_TRUSTED_CA)
		xklistinit(xlist, DANE_CERT_LIST, x, X509_free);
	    break;

	case SSL_DANE_SELECTOR_SPKI:
	    if (!d2i_PUBKEY(&k, &p, dlen) || dlen != p - data) {
		if (k)
		    EVP_PKEY_free(k);
		DANEerr(DANE_F_SSL_DANE_ADD_TLSA, DANE_R_BAD_PKEY);
		return 0;
	    }
	    if (usage == SSL_DANE_USAGE_TRUSTED_CA)
		xklistinit(klist, DANE_PKEY_LIST, k, EVP_PKEY_free);
	    break;
	}
    }

    /* Find insertion point and don't add duplicate elements. */
    for (s = dane->selectors[usage]; s; s = s->next)
	if (s->value->selector == selector)
	    for (m = s->value->mtype; m; m = m->next)
		if (m->value->md == md)
		    for (d = m->value->data; d; d = d->next)
			if (d->value->datalen == dlen &&
			    memcmp(d->value->data, data, dlen) == 0)
			    xkfreeret(1);

    if ((d = (DANE_DATA_LIST) list_alloc(sizeof(*d->value) + dlen)) == 0)
	xkfreeret(0);
    d->value->datalen = dlen;
    memcpy(d->value->data, data, dlen);
    if (!m) {
	if ((m = (DANE_MTYPE_LIST) list_alloc(sizeof(*m->value))) == 0) {
	    list_free(d, OPENSSL_freeFunc);
	    xkfreeret(0);
	}
	m->value->data = 0;
	if ((m->value->md = md) != 0)
	    m->value->mdlen = dlen;
	if (!s) {
	    if ((s = (DANE_SELECTOR_LIST) list_alloc(sizeof(*s->value))) == 0) {
		list_free(m, dane_mtype_free);
		xkfreeret(0);
	    }
	    s->value->mtype = 0;
	    s->value->selector = selector;
	    LINSERT(dane->selectors[usage], s);
	}
	LINSERT(s->value->mtype, m);
    }
    LINSERT(m->value->data, d);

    if (xlist)
	LINSERT(dane->certs, xlist);
    else if (klist)
	LINSERT(dane->pkeys, klist);
    return 1;
}

int SSL_dane_init(SSL *ssl, const char *sni_domain, const char **hostnames)
{
    SSL_DANE *dane;
    int i;
#ifdef OPENSSL_INTERNAL
    SSL_CTX *sctx = SSL_get_SSL_CTX(ssl);

    if (sctx->app_verify_callback != verify_cert) {
	DANEerr(DANE_F_SSL_DANE_INIT, DANE_R_SCTX_INIT);
	return -1;
    }
#else
    if (dane_idx < 0) {
	DANEerr(DANE_F_SSL_DANE_INIT, DANE_R_LIBRARY_INIT);
	return -1;
    }
#endif

    if (sni_domain && !SSL_set_tlsext_host_name(ssl, sni_domain))
	return 0;

    if ((dane = (SSL_DANE *) OPENSSL_malloc(sizeof(SSL_DANE))) == 0) {
	DANEerr(DANE_F_SSL_DANE_INIT, ERR_R_MALLOC_FAILURE);
	return 0;
    }
    if (!SSL_set_ex_data(ssl, dane_idx, dane)) {
	DANEerr(DANE_F_SSL_DANE_INIT, ERR_R_MALLOC_FAILURE);
	OPENSSL_free(dane);
	return 0;
    }

    dane->pkeys = 0;
    dane->certs = 0;
    dane->chain = 0;
    dane->roots = 0;
    dane->depth = -1;
    dane->mhost = 0;
    dane->multi = 0;

    for (i = 0; i <= SSL_DANE_USAGE_LAST; ++i)
	dane->selectors[i] = 0;

    if (hostnames && (dane->hosts = host_list_init(hostnames)) == 0) {
	DANEerr(DANE_F_SSL_DANE_INIT, ERR_R_MALLOC_FAILURE);
	SSL_dane_cleanup(ssl);
	return 0;
    }

    return 1;
}

int SSL_CTX_dane_init(SSL_CTX *ctx)
{
    if (dane_idx >= 0) {
	SSL_CTX_set_cert_verify_callback(ctx, verify_cert, 0);
	return 1;
    }
    DANEerr(DANE_F_SSL_CTX_DANE_INIT, DANE_R_LIBRARY_INIT);
    return -1;
}

int SSL_dane_library_init(void)
{
    if (err_lib_dane < 0)
	safe_init(&err_lib_dane, ERR_get_next_error_library, dane_init);

    if (dane_idx >= 0) {
	if (!wrap_signed || (signmd && signkey))
	    return 1;			/* Full DANE support */
	return 0;			/* DANE usage 2 unavailable */
    }
    DANEerr(DANE_F_SSL_DANE_LIBRARY_INIT, DANE_R_DANE_SUPPORT);
    return -1;				/* No DANE support. */
}
