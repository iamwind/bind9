/*
 * Portions Copyright (C) 2004, 2005  Internet Systems Consortium, Inc. ("ISC")
 * Portions Copyright (C) 2000-2003  Internet Software Consortium.
 * Portions Copyright (C) 1995-2000 by Network Associates, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC AND NETWORK ASSOCIATES DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE
 * FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id: dnssec-signkey.c,v 1.62.18.3 2005/04/29 00:15:21 marka Exp $ */

/*! \file */

/*% dnssec-signkey - DNSSEC key set signing tool
 * 
 * \section dnssec-signkey-synopsis SYNOPSIS
 * \par
 *        dnssec-signkey [ -a ]  [ -c class ]  [ -s start-time ]  [ -e end-time ]
 *        [ -h ]  [ -p ]  [ -r randomdev ]  [ -v level ]  keyset key...
 * 
 * \section dnssec-signkey-description DESCRIPTION
 * \par
 *        dnssec-signkey signs a keyset. Typically the keyset will be for a child
 *        zone,  and  will  have  been  generated by dnssec-makekeyset. The child
 *        zone's keyset is signed with the zone keys for  its  parent  zone.  The
 *        output  file  is  of  the  form signedkey-nnnn., where nnnn is the zone
 *        name.
 * 
 * \link org.isc.doc.0038 More ... \endlink
 */

/** \page org.isc.doc.0038 dnssec-signkey
 *  \htmlinclude org.isc.doc.0038.html
 */

#include <config.h>

#include <stdlib.h>

#include <isc/string.h>
#include <isc/commandline.h>
#include <isc/entropy.h>
#include <isc/mem.h>
#include <isc/print.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/diff.h>
#include <dns/dnssec.h>
#include <dns/fixedname.h>
#include <dns/log.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdatastruct.h>
#include <dns/result.h>
#include <dns/secalg.h>

#include <dst/dst.h>

#include "dnssectool.h"

const char *program = "dnssec-signkey";
int verbose;

typedef struct keynode keynode_t;
struct keynode {
	dst_key_t *key;
	isc_boolean_t verified;
	ISC_LINK(keynode_t) link;
};
typedef ISC_LIST(keynode_t) keylist_t;

static isc_stdtime_t starttime = 0, endtime = 0, now;

static isc_mem_t *mctx = NULL;
static isc_entropy_t *ectx = NULL;
static keylist_t keylist;

static void
usage(void) {
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s [options] keyset keys\n", program);

	fprintf(stderr, "\n");

	fprintf(stderr, "Version: %s\n", VERSION);

	fprintf(stderr, "Options: (default value in parenthesis) \n");
	fprintf(stderr, "\t-a\n");
	fprintf(stderr, "\t\tverify generated signatures\n");
	fprintf(stderr, "\t-c class (IN)\n");
	fprintf(stderr, "\t-s YYYYMMDDHHMMSS|+offset:\n");
	fprintf(stderr, "\t\tSIG start time - absolute|offset (from keyset)\n");
	fprintf(stderr, "\t-e YYYYMMDDHHMMSS|+offset|\"now\"+offset]:\n");
	fprintf(stderr, "\t\tSIG end time  - absolute|from start|from now "
		"(from keyset)\n");
	fprintf(stderr, "\t-v level:\n");
	fprintf(stderr, "\t\tverbose level (0)\n");
	fprintf(stderr, "\t-p\n");
	fprintf(stderr, "\t\tuse pseudorandom data (faster but less secure)\n");
	fprintf(stderr, "\t-r randomdev:\n");
	fprintf(stderr, "\t\ta file containing random data\n");

	fprintf(stderr, "\n");

	fprintf(stderr, "keyset:\n");
	fprintf(stderr, "\tfile with keyset to be signed (keyset-<name>)\n");
	fprintf(stderr, "keys:\n");
	fprintf(stderr, "\tkeyfile (Kname+alg+tag)\n");

	fprintf(stderr, "\n");
	fprintf(stderr, "Output:\n");
	fprintf(stderr, "\tsigned keyset (signedkey-<name>)\n");
	exit(0);
}

static void
loadkeys(dns_name_t *name, dns_rdataset_t *rdataset) {
	dst_key_t *key;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	keynode_t *keynode;
	isc_result_t result;

	ISC_LIST_INIT(keylist);
	result = dns_rdataset_first(rdataset);
	check_result(result, "dns_rdataset_first");
	for (; result == ISC_R_SUCCESS; result = dns_rdataset_next(rdataset)) {
		dns_rdata_reset(&rdata);
		dns_rdataset_current(rdataset, &rdata);
		key = NULL;
		result = dns_dnssec_keyfromrdata(name, &rdata, mctx, &key);
		if (result != ISC_R_SUCCESS)
			continue;
		if (!dst_key_iszonekey(key)) {
			dst_key_free(&key);
			continue;
		}
		keynode = isc_mem_get(mctx, sizeof(keynode_t));
		if (keynode == NULL)
			fatal("out of memory");
		keynode->key = key;
		keynode->verified = ISC_FALSE;
		ISC_LIST_INITANDAPPEND(keylist, keynode, link);
	}
	if (result != ISC_R_NOMORE)
		fatal("failure traversing key list");
}

static dst_key_t *
findkey(dns_rdata_rrsig_t *sig) {
	keynode_t *keynode;
	for (keynode = ISC_LIST_HEAD(keylist);
	     keynode != NULL;
	     keynode = ISC_LIST_NEXT(keynode, link))
	{
		if (dst_key_id(keynode->key) == sig->keyid &&
		    dst_key_alg(keynode->key) == sig->algorithm) {
			keynode->verified = ISC_TRUE;
			return (keynode->key);
		}
	}
	fatal("signature generated by non-zone or missing key");
	return (NULL);
}

int
main(int argc, char *argv[]) {
	int i, ch;
	char *startstr = NULL, *endstr = NULL, *classname = NULL;
	char tdomain[1025];
	dns_fixedname_t fdomain;
	dns_name_t *domain;
	char *output = NULL;
	char *endp;
	unsigned char data[65536];
	dns_db_t *db;
	dns_dbnode_t *node;
	dns_dbversion_t *version;
	dns_diff_t diff;
	dns_difftuple_t *tuple;
	dns_dbiterator_t *dbiter;
	dns_rdatasetiter_t *rdsiter;
	dst_key_t *key = NULL;
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_t sigrdata = DNS_RDATA_INIT;
	dns_rdataset_t rdataset, sigrdataset;
	dns_rdata_rrsig_t sig;
	isc_result_t result;
	isc_buffer_t b;
	isc_log_t *log = NULL;
	keynode_t *keynode;
	isc_boolean_t pseudorandom = ISC_FALSE;
	unsigned int eflags;
	dns_rdataclass_t rdclass;
	isc_boolean_t tryverify = ISC_FALSE;
	isc_boolean_t settime = ISC_FALSE;

	result = isc_mem_create(0, 0, &mctx);
	check_result(result, "isc_mem_create()");

	dns_result_register();

	while ((ch = isc_commandline_parse(argc, argv, "ac:s:e:pr:v:h")) != -1)
	{
		switch (ch) {
		case 'a':
			tryverify = ISC_TRUE;
			break;
		case 'c':
			classname = isc_commandline_argument;
			break;

		case 's':
			startstr = isc_commandline_argument;
			break;
						
		case 'e':
			endstr = isc_commandline_argument;
			break;

		case 'p':
			pseudorandom = ISC_TRUE;
			break;

		case 'r':
			setup_entropy(mctx, isc_commandline_argument, &ectx);
			break;

		case 'v':
			endp = NULL;
			verbose = strtol(isc_commandline_argument, &endp, 0);
			if (*endp != '\0')
				fatal("verbose level must be numeric");
			break;

		case 'h':
		default:
			usage();

		}
	}

	argc -= isc_commandline_index;
	argv += isc_commandline_index;

	if (argc < 2)
		usage();

	rdclass = strtoclass(classname);

	if (ectx == NULL)
		setup_entropy(mctx, NULL, &ectx);
	eflags = ISC_ENTROPY_BLOCKING;
	if (!pseudorandom)
		eflags |= ISC_ENTROPY_GOODONLY;
	result = dst_lib_init(mctx, ectx, eflags);
	if (result != ISC_R_SUCCESS)
		fatal("could not initialize dst: %s", 
		      isc_result_totext(result));

	isc_stdtime_get(&now);

	if ((startstr == NULL || endstr == NULL) &&
	    !(startstr == NULL && endstr == NULL))
		fatal("if -s or -e is specified, both must be");

	if (startstr != NULL) {
		starttime = strtotime(startstr, now, now);
		endtime = strtotime(endstr, now, starttime);
		settime = ISC_TRUE;
	}

	setup_logging(verbose, mctx, &log);

	if (strlen(argv[0]) < 8U || strncmp(argv[0], "keyset-", 7) != 0)
		fatal("keyset file '%s' must start with keyset-", argv[0]);

	db = NULL;
	result = dns_db_create(mctx, "rbt", dns_rootname, dns_dbtype_zone,
			       rdclass, 0, NULL, &db);
	check_result(result, "dns_db_create()");

	result = dns_db_load(db, argv[0]);
	if (result != ISC_R_SUCCESS && result != DNS_R_SEENINCLUDE)
		fatal("failed to load database from '%s': %s", argv[0],
		      isc_result_totext(result));

	dns_fixedname_init(&fdomain);
	domain = dns_fixedname_name(&fdomain);

	dbiter = NULL;
	result = dns_db_createiterator(db, ISC_FALSE, &dbiter);
	check_result(result, "dns_db_createiterator()");

	result = dns_dbiterator_first(dbiter);
	check_result(result, "dns_dbiterator_first()");
	while (result == ISC_R_SUCCESS) {
		node = NULL;
		dns_dbiterator_current(dbiter, &node, domain);
		rdsiter = NULL;
		result = dns_db_allrdatasets(db, node, NULL, 0, &rdsiter);
		check_result(result, "dns_db_allrdatasets()");
		result = dns_rdatasetiter_first(rdsiter);
		dns_rdatasetiter_destroy(&rdsiter);
		if (result == ISC_R_SUCCESS)
			break;
		dns_db_detachnode(db, &node);
		result = dns_dbiterator_next(dbiter);
	}
	dns_dbiterator_destroy(&dbiter);
	if (result != ISC_R_SUCCESS)
		fatal("failed to find data in keyset file");

	isc_buffer_init(&b, tdomain, sizeof(tdomain) - 1);
	result = dns_name_tofilenametext(domain, ISC_FALSE, &b);
	check_result(result, "dns_name_tofilenametext()");
	isc_buffer_putuint8(&b, 0);

	output = isc_mem_allocate(mctx,
				  strlen("signedkey-") + strlen(tdomain) + 1);
	if (output == NULL)
		fatal("out of memory");
	sprintf(output, "signedkey-%s", tdomain);

	version = NULL;
	dns_db_newversion(db, &version);

	dns_rdataset_init(&rdataset);
	dns_rdataset_init(&sigrdataset);
	result = dns_db_findrdataset(db, node, version, dns_rdatatype_dnskey, 0,
				     0, &rdataset, &sigrdataset);
	if (result != ISC_R_SUCCESS) {
		char domainstr[DNS_NAME_FORMATSIZE];
		dns_name_format(domain, domainstr, sizeof(domainstr));
		fatal("failed to find rdataset '%s KEY': %s",
		      domainstr, isc_result_totext(result));
	}

	loadkeys(domain, &rdataset);

	dns_diff_init(mctx, &diff);

	if (!dns_rdataset_isassociated(&sigrdataset))
		fatal("no SIG KEY set present");

	result = dns_rdataset_first(&sigrdataset);
	check_result(result, "dns_rdataset_first()");
	do {
		dns_rdataset_current(&sigrdataset, &sigrdata);
		result = dns_rdata_tostruct(&sigrdata, &sig, mctx);
		check_result(result, "dns_rdata_tostruct()");
		key = findkey(&sig);
		result = dns_dnssec_verify(domain, &rdataset, key,
					   ISC_TRUE, mctx, &sigrdata);
		if (result != ISC_R_SUCCESS) {
			char keystr[KEY_FORMATSIZE];
			key_format(key, keystr, sizeof(keystr));
			fatal("signature by key '%s' did not verify: %s",
			      keystr, isc_result_totext(result));
		}
		if (!settime) {
			starttime = sig.timesigned;
			endtime = sig.timeexpire;
			settime = ISC_TRUE;
		}
		dns_rdata_freestruct(&sig);
		dns_rdata_reset(&sigrdata);
		result = dns_rdataset_next(&sigrdataset);
	} while (result == ISC_R_SUCCESS);

	for (keynode = ISC_LIST_HEAD(keylist);
	     keynode != NULL;
	     keynode = ISC_LIST_NEXT(keynode, link))
		if (!keynode->verified)
			fatal("not all zone keys self signed the key set");

	argc -= 1;
	argv += 1;

	for (i = 0; i < argc; i++) {
		key = NULL;
		result = dst_key_fromnamedfile(argv[i],
					       DST_TYPE_PUBLIC |
					       DST_TYPE_PRIVATE,
					       mctx, &key);
		if (result != ISC_R_SUCCESS)
			fatal("failed to read key %s from disk: %s",
			      argv[i], isc_result_totext(result));

		dns_rdata_reset(&rdata);
		isc_buffer_init(&b, data, sizeof(data));
		result = dns_dnssec_sign(domain, &rdataset, key,
					 &starttime, &endtime,
					 mctx, &b, &rdata);
		isc_entropy_stopcallbacksources(ectx);
		if (result != ISC_R_SUCCESS) {
			char keystr[KEY_FORMATSIZE];
			key_format(key, keystr, sizeof(keystr));
			fatal("key '%s' failed to sign data: %s",
			      keystr, isc_result_totext(result));
		}
		if (tryverify) {
			result = dns_dnssec_verify(domain, &rdataset, key,
						   ISC_TRUE, mctx, &rdata);
			if (result != ISC_R_SUCCESS) {
				char keystr[KEY_FORMATSIZE];
				key_format(key, keystr, sizeof(keystr));
				fatal("signature from key '%s' failed to "
				      "verify: %s",
				      keystr, isc_result_totext(result));
			}
		}
		tuple = NULL;
		result = dns_difftuple_create(mctx, DNS_DIFFOP_ADD,
					      domain, rdataset.ttl,
					      &rdata, &tuple);
		check_result(result, "dns_difftuple_create");
		dns_diff_append(&diff, &tuple);
		dst_key_free(&key);
	}

	result = dns_db_deleterdataset(db, node, version, dns_rdatatype_rrsig,
				       dns_rdatatype_dnskey);
	check_result(result, "dns_db_deleterdataset");

	result = dns_diff_apply(&diff, db, version);
	check_result(result, "dns_diff_apply");
	dns_diff_clear(&diff);

	dns_db_detachnode(db, &node);
	dns_db_closeversion(db, &version, ISC_TRUE);
	result = dns_db_dump(db, version, output);
	if (result != ISC_R_SUCCESS)
		fatal("failed to write database to '%s': %s",
		      output, isc_result_totext(result));

	printf("%s\n", output);

	dns_rdataset_disassociate(&rdataset);
	dns_rdataset_disassociate(&sigrdataset);

	dns_db_detach(&db);

	while (!ISC_LIST_EMPTY(keylist)) {
		keynode = ISC_LIST_HEAD(keylist);
		ISC_LIST_UNLINK(keylist, keynode, link);
		dst_key_free(&keynode->key);
		isc_mem_put(mctx, keynode, sizeof(keynode_t));
	}

	cleanup_logging(&log);

	isc_mem_free(mctx, output);
	cleanup_entropy(&ectx);
	dst_lib_destroy();
	if (verbose > 10)
		isc_mem_stats(mctx, stdout);
	isc_mem_destroy(&mctx);
	return (0);
}
