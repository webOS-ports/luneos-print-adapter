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

#ifndef PRINTER_DB_H_
#define PRINTER_DB_H_

#include <glib.h>
#include <stdbool.h>

/*
 * Persistence for manually added printers and the current-printer selection.
 *
 * The original used SQLite; a GKeyFile is a better fit for a list this small
 * and drops a dependency. One group per printer, keyed by printerID.
 *
 * Printers are tagged with the SSID they were added on, because the webOS UI
 * groups them by network and the replicated UI expects the same. Discovered
 * (mDNS) printers are never stored - they come and go with the browse.
 */

struct printer_record {
	char *printer_id;
	char *printer_name;
	char *printer_address;
	char *ssid;
};

struct printer_db;

struct printer_db *printer_db_open(const char *path);
void printer_db_close(struct printer_db *db);

void printer_record_free(struct printer_record *rec);

/*
 * Returns false and sets *err to one of the PM_ERR_PRINTER_DUPLICATE_*
 * codes when the id or the address is already present.
 */
bool printer_db_add(struct printer_db *db, const char *printer_id,
                    const char *printer_name, const char *printer_address,
                    const char *ssid, int *err);

bool printer_db_delete(struct printer_db *db, const char *printer_id,
                       const char *ssid);

/* Caller owns the list and its records; ssid_filter NULL means every network. */
GList *printer_db_list(struct printer_db *db, const char *ssid_filter);

/* Borrowed pointer, valid until the next mutation. */
const struct printer_record *printer_db_find(struct printer_db *db,
                                             const char *printer_id);

const char *printer_db_get_current(struct printer_db *db);
bool printer_db_set_current(struct printer_db *db, const char *printer_id);

#endif

// vim:ts=4:sw=4:noexpandtab
