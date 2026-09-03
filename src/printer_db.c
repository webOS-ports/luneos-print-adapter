/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <string.h>
#include <errno.h>

#include "printer_db.h"
#include "print_errors.h"

#define GROUP_SETTINGS	"settings"
#define KEY_CURRENT	"currentPrinter"

struct printer_db {
	char *path;
	GKeyFile *keyfile;
	struct printer_record scratch;	/* backing store for printer_db_find */
	char *current_cache;		/* backing store for printer_db_get_current */
};

void printer_record_free(struct printer_record *rec)
{
	if (!rec)
		return;

	g_free(rec->printer_id);
	g_free(rec->printer_name);
	g_free(rec->printer_address);
	g_free(rec->ssid);
	g_free(rec);
}

static void scratch_clear(struct printer_db *db)
{
	g_clear_pointer(&db->scratch.printer_id, g_free);
	g_clear_pointer(&db->scratch.printer_name, g_free);
	g_clear_pointer(&db->scratch.printer_address, g_free);
	g_clear_pointer(&db->scratch.ssid, g_free);
}

struct printer_db *printer_db_open(const char *path)
{
	struct printer_db *db;
	char *dir;

	db = g_new0(struct printer_db, 1);
	db->path = g_strdup(path);
	db->keyfile = g_key_file_new();

	dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0755) != 0)
		g_warning("Could not create %s: %s", dir, g_strerror(errno));
	g_free(dir);

	/*
	 * A missing file is the normal first-boot case, not an error. A file
	 * that exists but will not parse is worth complaining about; we carry
	 * on with an empty store rather than refusing to start, since a print
	 * service that will not start is worse than one that forgot a printer.
	 */
	if (!g_key_file_load_from_file(db->keyfile, path, G_KEY_FILE_NONE, NULL))
		g_debug("No existing printer database at %s", path);

	return db;
}

void printer_db_close(struct printer_db *db)
{
	if (!db)
		return;

	scratch_clear(db);
	g_free(db->current_cache);
	g_key_file_free(db->keyfile);
	g_free(db->path);
	g_free(db);
}

static bool db_flush(struct printer_db *db)
{
	GError *error = NULL;

	if (!g_key_file_save_to_file(db->keyfile, db->path, &error)) {
		g_warning("Could not write printer database %s: %s", db->path,
		          error->message);
		g_error_free(error);
		return false;
	}

	return true;
}

static bool group_is_printer(const char *group)
{
	return strcmp(group, GROUP_SETTINGS) != 0;
}

bool printer_db_add(struct printer_db *db, const char *printer_id,
                    const char *printer_name, const char *printer_address,
                    const char *ssid, int *err)
{
	gchar **groups;
	gsize n, i;

	if (g_key_file_has_group(db->keyfile, printer_id)) {
		*err = PM_ERR_PRINTER_DUPLICATE_ID;
		return false;
	}

	/*
	 * The original distinguished "address already added by hand" from
	 * "address already discovered over zeroconf" with two error codes.
	 * Only manually added printers land in this store, so a hit here is
	 * always the manual one.
	 */
	groups = g_key_file_get_groups(db->keyfile, &n);
	for (i = 0; i < n; i++) {
		char *addr;

		if (!group_is_printer(groups[i]))
			continue;

		addr = g_key_file_get_string(db->keyfile, groups[i],
		                             "printerAddress", NULL);
		if (addr && printer_address && !strcmp(addr, printer_address)) {
			g_free(addr);
			g_strfreev(groups);
			*err = PM_ERR_PRINTER_DUPLICATE_IP;
			return false;
		}
		g_free(addr);
	}
	g_strfreev(groups);

	g_key_file_set_string(db->keyfile, printer_id, "printerName",
	                      printer_name ? printer_name : printer_id);
	g_key_file_set_string(db->keyfile, printer_id, "printerAddress",
	                      printer_address ? printer_address : "");
	g_key_file_set_string(db->keyfile, printer_id, "ssid", ssid ? ssid : "");

	if (!db_flush(db)) {
		g_key_file_remove_group(db->keyfile, printer_id, NULL);
		*err = PM_ERR_PRINTER_DB_ACCESS;
		return false;
	}

	return true;
}

bool printer_db_delete(struct printer_db *db, const char *printer_id,
                       const char *ssid)
{
	char *current;

	if (!g_key_file_has_group(db->keyfile, printer_id))
		return false;

	if (ssid && *ssid) {
		char *stored = g_key_file_get_string(db->keyfile, printer_id,
		                                     "ssid", NULL);
		bool match = stored && !strcmp(stored, ssid);

		g_free(stored);
		if (!match)
			return false;
	}

	if (!g_key_file_remove_group(db->keyfile, printer_id, NULL))
		return false;

	/* Don't leave currentPrinter pointing at something we just removed. */
	current = g_key_file_get_string(db->keyfile, GROUP_SETTINGS,
	                                KEY_CURRENT, NULL);
	if (current && !strcmp(current, printer_id))
		g_key_file_remove_key(db->keyfile, GROUP_SETTINGS, KEY_CURRENT,
		                      NULL);
	g_free(current);

	return db_flush(db);
}

GList *printer_db_list(struct printer_db *db, const char *ssid_filter)
{
	GList *out = NULL;
	gchar **groups;
	gsize n, i;

	groups = g_key_file_get_groups(db->keyfile, &n);

	for (i = 0; i < n; i++) {
		struct printer_record *rec;
		char *ssid;

		if (!group_is_printer(groups[i]))
			continue;

		ssid = g_key_file_get_string(db->keyfile, groups[i], "ssid",
		                             NULL);

		if (ssid_filter && strcmp(ssid_filter, ssid ? ssid : "")) {
			g_free(ssid);
			continue;
		}

		rec = g_new0(struct printer_record, 1);
		rec->printer_id = g_strdup(groups[i]);
		rec->printer_name = g_key_file_get_string(db->keyfile,
		                                          groups[i],
		                                          "printerName", NULL);
		rec->printer_address = g_key_file_get_string(db->keyfile,
		                                             groups[i],
		                                             "printerAddress",
		                                             NULL);
		rec->ssid = ssid;

		out = g_list_prepend(out, rec);
	}

	g_strfreev(groups);

	return g_list_reverse(out);
}

const struct printer_record *printer_db_find(struct printer_db *db,
                                             const char *printer_id)
{
	if (!printer_id || !g_key_file_has_group(db->keyfile, printer_id))
		return NULL;

	scratch_clear(db);

	db->scratch.printer_id = g_strdup(printer_id);
	db->scratch.printer_name = g_key_file_get_string(db->keyfile,
	                                                 printer_id,
	                                                 "printerName", NULL);
	db->scratch.printer_address = g_key_file_get_string(db->keyfile,
	                                                    printer_id,
	                                                    "printerAddress",
	                                                    NULL);
	db->scratch.ssid = g_key_file_get_string(db->keyfile, printer_id,
	                                         "ssid", NULL);

	return &db->scratch;
}

const char *printer_db_get_current(struct printer_db *db)
{
	g_free(db->current_cache);
	db->current_cache = g_key_file_get_string(db->keyfile, GROUP_SETTINGS,
	                                          KEY_CURRENT, NULL);

	return db->current_cache ? db->current_cache : "";
}

bool printer_db_set_current(struct printer_db *db, const char *printer_id)
{
	g_key_file_set_string(db->keyfile, GROUP_SETTINGS, KEY_CURRENT,
	                      printer_id ? printer_id : "");

	return db_flush(db);
}

// vim:ts=4:sw=4:noexpandtab
