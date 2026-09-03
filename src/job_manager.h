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

#ifndef JOB_MANAGER_H_
#define JOB_MANAGER_H_

#include <glib.h>
#include <stdbool.h>

/*
 * Job objects, mirroring the webOS lifecycle:
 *
 *   open -> editPrintParams -> getFinalParamsAndArea -> addFile* -> close
 *
 * A job accumulates files while open and is handed to CUPS as a single job on
 * close. That matches what the client library expects (it waits for a terminal
 * getStatus after close) and it means multi-page documents arrive as one queue
 * entry rather than N.
 */

/* Print parameters in webOS keyword terms. */
struct print_params {
	char *media_size;
	char *media_type;
	char *print_quality;
	char *duplex;
	char *dry_time;
	char *color;
	char *tray;
	int num_copies;
	bool borderless;
	bool auto_rotate;
	bool auto_scale;
	double top_inset;
	double left_inset;
	double right_inset;
	double bottom_inset;
	bool insets_set;
};

enum job_phase {
	JOB_PHASE_OPEN,		/* accepting params and files */
	JOB_PHASE_SUBMITTED,	/* handed to CUPS, awaiting terminal state */
	JOB_PHASE_DONE,
	JOB_PHASE_CANCELLED,
};

struct print_job {
	int job_id;		/* webOS jobID, small and monotonic */
	int cups_job_id;	/* -1 until submitted */
	char *printer_id;
	char *description;
	char *app_name;
	struct print_params params;
	GList *files;		/* char*, in page order */
	GList *temp_files;	/* char*, ours to unlink */
	char *temp_dir;
	enum job_phase phase;
	int render_result_code;
	bool render_result_set;
	char *render_result_text;
	int current_page;
	int total_pages;
	/* Last reported state, so we only post subscriptions on change. */
	char *last_printer_state;
	gint64 queued_at;
	gint64 started_at;
};

struct job_manager;

struct job_manager *job_manager_new(const char *spool_root);
void job_manager_free(struct job_manager *jm);

struct print_job *job_manager_open(struct job_manager *jm,
                                   const char *printer_id,
                                   const char *description,
                                   const char *app_name);

struct print_job *job_manager_find(struct job_manager *jm, int job_id);
GList *job_manager_list(struct job_manager *jm);	/* borrowed */

void job_manager_remove(struct job_manager *jm, int job_id);

/* Allocates a temp file inside the job's spool dir. NULL on failure. */
char *job_manager_new_temp_file(struct job_manager *jm, struct print_job *job,
                                const char *extension, gint64 size, int *err);

bool job_manager_resize_temp_file(struct print_job *job, const char *path,
                                  gint64 new_size, int *err);

/* Defaults matching what the shipped daemon reported for a fresh job. */
void print_params_init(struct print_params *p);
void print_params_clear(struct print_params *p);

/*
 * Printable geometry, in the shape the real daemon returned:
 * pixel dimensions at pixel_units dpi, plus the hardware margins in inches.
 */
struct printable_area {
	int pixel_units;
	int raw_width;
	int raw_height;
	int width;
	int height;
	double page_width;
	double page_height;
	double page_top_margin;
	double page_left_margin;
	double page_right_margin;
	double page_bottom_margin;
	bool params_adjusted;
};

/*
 * Computes the imageable area for a media size and inset set. Returns false
 * with *err set when the insets leave no printable region (-613).
 */
bool job_compute_area(const char *media_size, const struct print_params *p,
                      int pixel_units, struct printable_area *out, int *err);

#endif

// vim:ts=4:sw=4:noexpandtab
