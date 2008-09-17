/*
 * Central Regulatory Domain Agent for Linux
 *
 * Userspace helper which sends regulatory domains to Linux via nl80211
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>  
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <linux/nl80211.h>

#include "regdb.h"

#ifdef USE_OPENSSL
#include <openssl/objects.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include "keys-ssl.c"
#endif

#ifdef USE_GCRYPT
#include <gcrypt.h>

#include "keys-gcrypt.c"
#endif

struct nl80211_state {
	struct nl_handle *nl_handle;
	struct nl_cache *nl_cache;
	struct genl_family *nl80211;
};

static int nl80211_init(struct nl80211_state *state)
{
	int err;

	state->nl_handle = nl_handle_alloc();
	if (!state->nl_handle) {
		fprintf(stderr, "Failed to allocate netlink handle.\n");
		return -ENOMEM;
	}

	if (genl_connect(state->nl_handle)) {
		fprintf(stderr, "Failed to connect to generic netlink.\n");
		err = -ENOLINK;
		goto out_handle_destroy;
	}

	state->nl_cache = genl_ctrl_alloc_cache(state->nl_handle);
	if (!state->nl_cache) {
		fprintf(stderr, "Failed to allocate generic netlink cache.\n");
		err = -ENOMEM;
		goto out_handle_destroy;
	}

	state->nl80211 = genl_ctrl_search_by_name(state->nl_cache, "nl80211");
	if (!state->nl80211) {
		fprintf(stderr, "nl80211 not found.\n");
		err = -ENOENT;
		goto out_cache_free;
	}

	return 0;

 out_cache_free:
	nl_cache_free(state->nl_cache);
 out_handle_destroy:
	nl_handle_destroy(state->nl_handle);
	return err;
}

static void nl80211_cleanup(struct nl80211_state *state)
{
	genl_family_put(state->nl80211);
	nl_cache_free(state->nl_cache);
	nl_handle_destroy(state->nl_handle);
}

static int reg_handler(struct nl_msg *msg, void *arg)
{
	printf("=== reg_handler() called\n");
	return NL_SKIP;
}

static int wait_handler(struct nl_msg *msg, void *arg)
{
	int *finished = arg;
	*finished = 1;
	return NL_STOP;
}


static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
	fprintf(stderr, "nl80211 error %d\n", err->error);
	exit(err->error);
}

int isalpha_upper(char letter)
{
	if (letter >= 65 && letter <= 90)
		return 1;
	return 0;
}

static int is_alpha2(char *alpha2)
{
	if (isalpha_upper(alpha2[0]) && isalpha_upper(alpha2[1]))
		return 1;
	return 0;
}

static int is_world_regdom(char *alpha2)
{
	/* ASCII 0 */
	if (alpha2[0] == 48 && alpha2[1] == 48)
		return 1;
	return 0;
}

static void *get_file_ptr(__u8 *db, int dblen, int structlen, __be32 ptr)
{
	__u32 p = ntohl(ptr);

	if (p > dblen - structlen) {
		fprintf(stderr, "Invalid database file, bad pointer!\n");
		exit(3);
	}

	return (void *)(db + p);
}

static int put_reg_rule(__u8 *db, int dblen, __be32 ruleptr, struct nl_msg *msg)
{
	struct regdb_file_reg_rule *rule;
	struct regdb_file_freq_range *freq;
	struct regdb_file_power_rule *power;

	rule	= get_file_ptr(db, dblen, sizeof(*rule), ruleptr);
	freq	= get_file_ptr(db, dblen, sizeof(*freq), rule->freq_range_ptr);
	power	= get_file_ptr(db, dblen, sizeof(*power), rule->power_rule_ptr);

	NLA_PUT_U32(msg, NL80211_ATTR_REG_RULE_FLAGS,		ntohl(rule->flags));
	NLA_PUT_U32(msg, NL80211_ATTR_FREQ_RANGE_START,		ntohl(freq->start_freq));
	NLA_PUT_U32(msg, NL80211_ATTR_FREQ_RANGE_END,		ntohl(freq->end_freq));
	NLA_PUT_U32(msg, NL80211_ATTR_FREQ_RANGE_MAX_BW,	ntohl(freq->max_bandwidth));
	NLA_PUT_U32(msg, NL80211_ATTR_POWER_RULE_MAX_ANT_GAIN,	ntohl(power->max_antenna_gain));
	NLA_PUT_U32(msg, NL80211_ATTR_POWER_RULE_MAX_EIRP,	ntohl(power->max_eirp));

	return 0;

nla_put_failure:
	return -1;
}

int main(int argc, char **argv)
{
	int fd;
	struct stat stat;
	__u8 *db;
	struct regdb_file_header *header;
	struct regdb_file_reg_country *countries;
	int dblen, siglen, num_countries, i, j, r;
	char alpha2[2];
	char *env_country;
	struct nl80211_state nlstate;
	struct nl_cb *cb = NULL;
	struct nl_msg *msg;
	int found_country = 0;
	int finished = 0;

	struct regdb_file_reg_rules_collection *rcoll;
	struct regdb_file_reg_country *country;
	struct nlattr *nl_reg_rules;
	int num_rules;

#ifdef USE_OPENSSL
	RSA *rsa;
	__u8 hash[SHA_DIGEST_LENGTH];
	int ok = 0;
#endif
#ifdef USE_GCRYPT
	/* initialise */
	gcry_check_version(NULL);

	gcry_mpi_t mpi_e, mpi_n;
	gcry_sexp_t rsa, signature, data;
	__u8 hash[20];
	int ok = 0;
#endif
	char *regdb = "/usr/lib/crda/regulatory.bin";

	if (argc != 1) {
		fprintf(stderr, "Usage: %s\n", argv[0]);
		return -EINVAL;
	}

	env_country = getenv("COUNTRY");
	if (!env_country) {
		fprintf(stderr, "COUNTRY environment variable not set.\n");
		return -EINVAL;
	}

	
	if (!is_alpha2(env_country) && !is_world_regdom(env_country)) {
		fprintf(stderr, "Invalid alpha2 set in COUNTRY\n");
		return -EINVAL;
	}

	memcpy(alpha2, env_country, 2);

	r = nl80211_init(&nlstate);
	if (r)
		return -EIO;

	fd = open(regdb, O_RDONLY);
	if (fd < 0) {
		perror("failed to open db file");
		r = -ENOENT;
		goto out;
	}

	if (fstat(fd, &stat)) {
		perror("failed to fstat db file");
		r = -EIO;
		goto out;
	}

	dblen = stat.st_size;

	db = mmap(NULL, dblen, PROT_READ, MAP_PRIVATE, fd, 0);
	if (db == MAP_FAILED) {
		perror("failed to mmap db file");
		r = -EIO;
		goto out;
	}

	header = get_file_ptr(db, dblen, sizeof(*header), 0);

	if (ntohl(header->magic) != REGDB_MAGIC) {
		fprintf(stderr, "Invalid database magic\n");
		r = -EINVAL;
		goto out;
	}

	if (ntohl(header->version) != REGDB_VERSION) {
		fprintf(stderr, "Invalid database version\n");
		r = -EINVAL;
		goto out;
	}

	siglen = ntohl(header->signature_length);
	/* adjust dblen so later sanity checks don't run into the signature */
	dblen -= siglen;

	if (dblen <= sizeof(*header)) {
		fprintf(stderr, "Invalid signature length %d\n", siglen);
		r = -EINVAL;
		goto out;
	}

	/* verify signature */
#ifdef USE_OPENSSL
	rsa = RSA_new();
	if (!rsa) {
		fprintf(stderr, "Failed to create RSA key\n");
		r = -EINVAL;
		goto out;
	}

	if (SHA1(db, dblen, hash) != hash) {
		fprintf(stderr, "Failed to calculate SHA sum\n");
		r = -EINVAL;
		goto out;
	}

	for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
		rsa->e = &keys[i].e;
		rsa->n = &keys[i].n;

		if (RSA_size(rsa) != siglen)
			continue;

		ok = RSA_verify(NID_sha1, hash, SHA_DIGEST_LENGTH,
				db + dblen, siglen, rsa) == 1;
		if (ok)
			break;
	}

	if (!ok) {
		fprintf(stderr, "Database signature wrong\n");
		r = -EINVAL;
		goto out;
	}

	rsa->e = NULL;
	rsa->n = NULL;
	RSA_free(rsa);

	BN_print_fp(stdout, &keys[0].n);

#endif

#ifdef USE_GCRYPT
	/* initialise */
	gcry_check_version(NULL);

	/* hash the db */
	gcry_md_hash_buffer(GCRY_MD_SHA1, hash, db, dblen);

	if (gcry_sexp_build(&data, NULL, "(data (flags pkcs1) (hash sha1 %b))",
			    20, hash)) {
		fprintf(stderr, "failed to build data expression\n");
		return 2;
	}

	if (gcry_sexp_build(&signature, NULL, "(sig-val (rsa (s %b)))",
			    siglen, db + dblen)) {
		fprintf(stderr, "failed to build signature expression\n");
		return 2;
	}

	for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
		if (gcry_mpi_scan(&mpi_e, GCRYMPI_FMT_USG,
				  keys[0].e, keys[0].len_e, NULL) ||
		    gcry_mpi_scan(&mpi_n, GCRYMPI_FMT_USG,
		    		  keys[0].n, keys[0].len_n, NULL)) {
			fprintf(stderr, "failed to convert numbers\n");
			return 2;
		}

		if (gcry_sexp_build(&rsa, NULL,
				    "(public-key (rsa (n %m) (e %m)))",
				    mpi_n, mpi_e)) {
			fprintf(stderr, "failed to build rsa key\n");
			return 2;
		}

		if (!gcry_pk_verify(signature, data, rsa)) {
			ok = 1;
			break;
		}
	}

	if (!ok) {
		fprintf(stderr, "Database signature wrong\n");
		return 2;
	}

#endif

	num_countries = ntohl(header->reg_country_num);
	countries = get_file_ptr(db, dblen,
				 sizeof(struct regdb_file_reg_country) * num_countries,
				 header->reg_country_ptr);

	for (i = 0; i < num_countries; i++) {
		country = countries + i;
		if (memcmp(country->alpha2, alpha2, 2) == 0) {
			found_country = 1;
			break;
		}
	}

	if (!found_country) {
		fprintf(stderr, "failed to find a country match in regulatory database\n");
		return -1;
	}

	msg = nlmsg_alloc();
	if (!msg) {
		fprintf(stderr, "failed to allocate netlink msg\n");
		return -1;
	}

	genlmsg_put(msg, 0, 0, genl_family_get_id(nlstate.nl80211), 0,
		0, NL80211_CMD_SET_REG, 0);


	rcoll = get_file_ptr(db, dblen, sizeof(*rcoll), country->reg_collection_ptr);
	num_rules = ntohl(rcoll->reg_rule_num);
	/* re-get pointer with sanity checking for num_rules */
	rcoll = get_file_ptr(db, dblen,
			     sizeof(*rcoll) + num_rules * sizeof(__be32),
			     country->reg_collection_ptr);

	NLA_PUT_STRING(msg, NL80211_ATTR_REG_ALPHA2, (char *) country->alpha2);

	nl_reg_rules = nla_nest_start(msg, NL80211_ATTR_REG_RULES);
	if (!nl_reg_rules) {
		r = -1;
		goto nla_put_failure;
	}

	for (j = 0; j < num_rules; j++) {
		struct nlattr *nl_reg_rule;
		nl_reg_rule = nla_nest_start(msg, i);
		if (!nl_reg_rule)
			goto nla_put_failure;

		r = put_reg_rule(db, dblen, rcoll->reg_rule_ptrs[j], msg);
		if (r)
			goto nla_put_failure;

		nla_nest_end(msg, nl_reg_rule);
	}

	nla_nest_end(msg, nl_reg_rules);

	cb = nl_cb_alloc(NL_CB_CUSTOM);
	if (!cb)
		goto cb_out;

	r = nl_send_auto_complete(nlstate.nl_handle, msg);

	if (r < 0) {
		fprintf(stderr, "failed to send regulatory request: %d\n", r);
		goto cb_out;
	}

	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, reg_handler, NULL);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, wait_handler, &finished);
	nl_cb_err(cb, NL_CB_CUSTOM, error_handler, NULL);

	if (!finished) {
		r = nl_wait_for_ack(nlstate.nl_handle);
		if (r < 0) {
			fprintf(stderr, "failed to set regulatory domain: %d\n", r);
			goto cb_out;
		}
	}

cb_out:
	nl_cb_put(cb);
nla_put_failure:
	nlmsg_free(msg);
out:
	nl80211_cleanup(&nlstate);
	return r;
}