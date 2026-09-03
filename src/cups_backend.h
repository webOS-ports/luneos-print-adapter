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

#ifndef CUPS_BACKEND_H_
#define CUPS_BACKEND_H_

#include <glib.h>
#include <stdbool.h>

/*
 * Everything that talks IPP lives here. CUPS does discovery (it merges local
 * queues with driverless printers found over mDNS), the filter chain, queueing
 * and retry; this file is only the translation surface.
 *
 * No webOS keyword appears below - callers map with param_map.
 */

struct cups_printer {
	char *printer_id;	/* CUPS dest name; the webOS printerID */
	char *printer_name;	/* printer-info, human readable */
	char *printer_address;	/* host part of device-uri */
	bool supports_ipp;
	bool is_discovered;	/* found over mDNS rather than a local queue */
};

struct cups_caps {
	GList *media_sizes;	/* char*, PWG names */
	GList *media_types;	/* char*, IPP keywords */
	GList *trays;		/* char*, IPP media-source keywords */
	bool can_duplex;
	bool has_photo_tray;
	bool can_borderless;
	bool quality_draft;
	bool quality_normal;
	bool quality_high;
	bool has_color;
	bool can_grayscale;
	bool can_cancel;
	bool is_supported;	/* speaks a document format we can feed it */
	int max_resolution;	/* dpi, 0 if not advertised */
};

/* Print parameters in IPP terms, ready to become job attributes. */
struct cups_job_params {
	const char *media;		/* PWG name, or NULL to omit */
	const char *media_type;		/* IPP keyword, or NULL */
	const char *media_source;	/* IPP keyword, or NULL */
	const char *sides;		/* IPP keyword, or NULL */
	const char *color_mode;		/* IPP keyword, or NULL */
	const char *print_scaling;	/* IPP keyword, or NULL */
	int quality;			/* 3/4/5, 0 to omit */
	int copies;
	int resolution;			/* dpi, 0 to omit */
	bool borderless;
	/* Margins in hundredths of a millimetre, IPP media-col units. */
	int top_margin;
	int left_margin;
	int right_margin;
	int bottom_margin;
	bool margins_valid;
};

struct cups_job_state {
	int ipp_state;		/* IPP job-state enum */
	GList *reasons;		/* char*, printer-state-reasons */
	bool is_toner;		/* marker-types says toner rather than ink */
	int current_page;	/* job-impressions-completed, -1 unknown */
	int total_pages;	/* -1 unknown */
};

void cups_printer_free(struct cups_printer *p);
void cups_caps_free(struct cups_caps *c);
void cups_job_state_free(struct cups_job_state *s);

/* GList of struct cups_printer*, caller owns. */
GList *cups_backend_enumerate(void);

/* NULL on failure, *err set to a PM_ERR_* code. */
struct cups_caps *cups_backend_get_caps(const char *printer_id, int *err);

/*
 * Submits every file as one job. Returns the CUPS job id, or -1 with *err set.
 * Files must already be in a format the printer accepts - see
 * cups_backend_format_supported.
 */
int cups_backend_submit(const char *printer_id, const char *title,
                        GList *files, const struct cups_job_params *params,
                        int *err);

bool cups_backend_cancel(const char *printer_id, int cups_job_id);

/* NULL if the job is no longer known to CUPS. */
struct cups_job_state *cups_backend_get_job_state(const char *printer_id,
                                                  int cups_job_id);

/* Sniffs content, not the extension. NULL if we cannot print it. */
const char *cups_backend_detect_format(const char *path);

#endif

// vim:ts=4:sw=4:noexpandtab
