/*-
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2014-2015 Matthew Seaman <matthew@FreeBSD.org>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pkg_config.h"

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <archive.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <khash.h>
#include <xstring.h>

#ifdef HAVE_SYS_CAPSICUM_H
#include <sys/capsicum.h>
#endif

#ifdef HAVE_CAPSICUM
#include <sys/capsicum.h>
#endif

#include <pkg.h>
#include "pkgcli.h"

void
usage_audit(void)
{
	fprintf(stderr, "Usage: pkg audit [-Fqr] [-f file] <pattern>\n\n");
	fprintf(stderr, "For more information see 'pkg help audit'.\n");
}

KHASH_MAP_INIT_STR(pkgs, struct pkg *);

static void
add_to_check(kh_pkgs_t *check, struct pkg *pkg)
{
	const char *uid;
	int ret;
	khint_t k;

	pkg_get(pkg, PKG_UNIQUEID, &uid);

	k = kh_put_pkgs(check, uid, &ret);
	if (ret != 0)
		kh_value(check, k) = pkg;
}

static void
print_recursive_rdeps(kh_pkgs_t *head, struct pkg *p, xstring *sb,
    kh_pkgs_t *seen, bool top)
{
	struct pkg_dep *dep = NULL;
	int ret;
	khint_t k, h;

	while(pkg_rdeps(p, &dep) == EPKG_OK) {
		const char *name = pkg_dep_get(dep, PKG_DEP_NAME);

		k = kh_get_pkgs(seen, name);
		if (k != kh_end(seen))
			continue;
		h = kh_get_pkgs(head, name);
		if (h == kh_end(head))
			continue;

		kh_put_pkgs(seen, name, &ret);
		if (!top)
			fprintf(sb->fp, ", ");

		fprintf(sb->fp, "%s", name);

		print_recursive_rdeps(head, kh_val(head, h), sb, seen, false);

		top = false;
	}
}

int
exec_audit(int argc, char **argv)
{
	struct pkg_audit	*audit;
	struct pkgdb		*db = NULL;
	struct pkgdb_it		*it = NULL;
	struct pkg		*pkg = NULL;
	char			*name;
	char			*version;
	char			*audit_file = NULL;
	unsigned int		 affected = 0, vuln = 0;
	bool			 fetch = false, recursive = false;
	int			 ch, i;
	int			 ret = EXIT_SUCCESS;
	xstring			*sb;
	kh_pkgs_t		*check = NULL;

	struct option longopts[] = {
		{ "fetch",	no_argument,		NULL,	'F' },
		{ "file",	required_argument,	NULL,	'f' },
		{ "recursive",	no_argument,	NULL,	'r' },
		{ "quiet",	no_argument,		NULL,	'q' },
		{ NULL,		0,			NULL,	0   },
	};

	while ((ch = getopt_long(argc, argv, "+Ff:qr", longopts, NULL)) != -1) {
		switch (ch) {
		case 'F':
			fetch = true;
			break;
		case 'f':
			audit_file = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 'r':
			recursive = true;
			break;
		default:
			usage_audit();
			return(EXIT_FAILURE);
		}
	}
	argc -= optind;
	argv += optind;

	audit = pkg_audit_new();

	if (fetch == true) {
		if (pkg_audit_fetch(NULL, audit_file) != EPKG_OK) {
			pkg_audit_free(audit);
			return (EXIT_FAILURE);
		}
	}

	if (pkg_audit_load(audit, audit_file) != EPKG_OK) {
		if (errno == ENOENT)
			warnx("vulnxml file %s does not exist. "
					"Try running 'pkg audit -F' first",
					audit_file);
		else
			warn("unable to open vulnxml file %s",
					audit_file);

		pkg_audit_free(audit);
		return (EXIT_FAILURE);
	}

	check = kh_init_pkgs();
	if (argc >= 1) {
		for (i = 0; i < argc; i ++) {
			name = argv[i];
			version = strrchr(name, '-');
			if (version != NULL) {
				version[0] = '\0';
				version++;
			}
			if (pkg_new(&pkg, PKG_FILE) != EPKG_OK)
				err(EXIT_FAILURE, "malloc");
			if (version != NULL)
				pkg_set(pkg, PKG_NAME, name, PKG_VERSION, version);
			else
				pkg_set(pkg, PKG_NAME, name);
			/* Fake uniqueid */
			pkg_set(pkg, PKG_UNIQUEID, name);
			add_to_check(check, pkg);
			pkg = NULL;
		}
	}
	else {

		/*
		 * if the database doesn't exist it just means there are no
		 * packages to audit.
		 */

		ret = pkgdb_access(PKGDB_MODE_READ, PKGDB_DB_LOCAL);
		if (ret == EPKG_ENODB) {
			pkg_audit_free(audit);
			kh_destroy_pkgs(check);
			return (EXIT_SUCCESS);
		} else if (ret == EPKG_ENOACCESS) {
			warnx("Insufficient privileges to read the package database");
			pkg_audit_free(audit);
			kh_destroy_pkgs(check);
			return (EXIT_FAILURE);
		} else if (ret != EPKG_OK) {
			warnx("Error accessing the package database");
			pkg_audit_free(audit);
			kh_destroy_pkgs(check);
			return (EXIT_FAILURE);
		}

		if (pkgdb_open(&db, PKGDB_DEFAULT) != EPKG_OK) {
			pkg_audit_free(audit);
			kh_destroy_pkgs(check);
			return (EXIT_FAILURE);
		}

		if (pkgdb_obtain_lock(db, PKGDB_LOCK_READONLY) != EPKG_OK) {
			pkgdb_close(db);
			pkg_audit_free(audit);
			kh_destroy_pkgs(check);
			warnx("Cannot get a read lock on a database, it is locked by another process");
			return (EXIT_FAILURE);
		}

		if ((it = pkgdb_query(db, NULL, MATCH_ALL)) == NULL) {
			warnx("Error accessing the package database");
			ret = EXIT_FAILURE;
		}
		else {
			while ((ret = pkgdb_it_next(it, &pkg, PKG_LOAD_BASIC|PKG_LOAD_RDEPS))
							== EPKG_OK) {
				add_to_check(check, pkg);
				pkg = NULL;
			}
			ret = EXIT_SUCCESS;
		}
		if (db != NULL) {
			pkgdb_it_free(it);
			pkgdb_release_lock(db, PKGDB_LOCK_READONLY);
			pkgdb_close(db);
		}
		if (ret != EXIT_SUCCESS) {
			pkg_audit_free(audit);
			kh_destroy_pkgs(check);
			return (ret);
		}
	}

	drop_privileges();

	/* Now we have vulnxml loaded and check list formed */
#ifdef HAVE_CAPSICUM
	if (cap_enter() < 0 && errno != ENOSYS) {
		warn("cap_enter() failed");
		pkg_audit_free(audit);
		kh_destroy_pkgs(check);
		return (EPKG_FATAL);
	}
#endif

	if (pkg_audit_process(audit) == EPKG_OK) {
		kh_foreach_value(check, pkg, {
			if (pkg_audit_is_vulnerable(audit, pkg, quiet, &sb, &affected)) {
				vuln ++;
				fflush(sb->fp);
				printf("%s", sb->buf);

				if (recursive) {
					const char *name;
					kh_pkgs_t *seen = kh_init_pkgs();

					pkg_get(pkg, PKG_NAME, &name);
					xstring_reset(sb);
					printf("Packages that depend on %s: ", name);
					fprintf(sb->fp, "Packages that depend on %s: ", name);
					fflush(sb->fp);
					print_recursive_rdeps(check, pkg , sb, seen, true);
					printf("%s\n\n", sb->buf);

					kh_destroy_pkgs(seen);
				}
				xstring_free(sb);
			}
			pkg_free(pkg);
		});
		kh_destroy_pkgs(check);

		if (ret == EPKG_END && vuln == 0)
			ret = EXIT_SUCCESS;

		if (!quiet)
			printf("%u problem(s) in %u installed package(s) found.\n",
			   affected, vuln);
	}
	else {
		warnx("cannot process vulnxml");
		ret = EXIT_FAILURE;
		kh_destroy_pkgs(check);
	}

	pkg_audit_free(audit);
	if (vuln != 0)
		ret = EXIT_FAILURE;

	return (ret);
}
