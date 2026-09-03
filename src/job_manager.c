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
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "job_manager.h"
#include "print_errors.h"

/* Hardware margin the original reported for every size it was asked about. */
#define DEFAULT_MARGIN_INCHES	0.125

struct job_manager {
	char *spool_root;
	GHashTable *jobs;	/* int job_id -> struct print_job* */
	int next_job_id;
};

/*
 * Physical media dimensions in inches, portrait. Metric sizes are converted
 * from their defining millimetre values rather than typed as rounded inches,
 * so A4 stays 210x297mm exactly.
 */
#define MM(x) ((x) / 25.4)

static const struct {
	const char *media;
	double width;
	double height;
} media_dims[] = {
	{ "US_Letter",			8.5,		11.0 },
	{ "US_Legal",			8.5,		14.0 },
	{ "B_Tabloid",			11.0,		17.0 },
	{ "Super B",			13.0,		19.0 },
	{ "ISO_A3",			MM(297.0),	MM(420.0) },
	{ "ISO_A4",			MM(210.0),	MM(297.0) },
	{ "ISO_A6",			MM(105.0),	MM(148.0) },
	{ "HAGAKI",			MM(100.0),	MM(148.0) },
	{ "Photo_L",			3.5,		5.0 },
	{ "Photo_4x6",			4.0,		6.0 },
	{ "Photo_5x7",			5.0,		7.0 },
	{ "Photo_5x7_MainTray",		5.0,		7.0 },
	{ "Photo_8x10",			8.0,		10.0 },
	{ "Photo_10x15",		MM(100.0),	MM(150.0) },
	{ "Photo_11x14",		11.0,		14.0 },
	{ "Photo_4x8",			4.0,		8.0 },
	{ "Photo_4X12",			4.0,		12.0 },
	{ "Printable CD 3.5 inch",	3.5,		3.5 },
	{ "Printable CD 5 inch",	MM(120.0),	MM(120.0) },
	{ NULL, 0, 0 },
};

void print_params_init(struct print_params *p)
{
	memset(p, 0, sizeof(*p));

	/*
	 * These are the exact defaults a freshly opened job reported on a
	 * TouchPad running 3.0.5 (jobs/getCurrentPrintParams before any
	 * editPrintParams), so a client that opens a job and immediately reads
	 * the params back sees what it always did.
	 */
	p->media_size = g_strdup("US_Letter");
	p->media_type = g_strdup("Plain");
	p->print_quality = g_strdup("Normal");
	p->duplex = g_strdup("None");
	p->dry_time = g_strdup("Normal");
	p->color = g_strdup("Color");
	p->tray = g_strdup("Auto");
	p->num_copies = 1;
	p->borderless = false;
	p->auto_rotate = false;
	p->auto_scale = false;
	p->top_inset = DEFAULT_MARGIN_INCHES;
	p->left_inset = DEFAULT_MARGIN_INCHES;
	p->right_inset = DEFAULT_MARGIN_INCHES;
	p->bottom_inset = DEFAULT_MARGIN_INCHES;
	p->insets_set = false;
}

void print_params_clear(struct print_params *p)
{
	if (!p)
		return;

	g_clear_pointer(&p->media_size, g_free);
	g_clear_pointer(&p->media_type, g_free);
	g_clear_pointer(&p->print_quality, g_free);
	g_clear_pointer(&p->duplex, g_free);
	g_clear_pointer(&p->dry_time, g_free);
	g_clear_pointer(&p->color, g_free);
	g_clear_pointer(&p->tray, g_free);
}

static bool media_lookup(const char *media, double *w, double *h)
{
	int i;

	if (!media)
		return false;

	for (i = 0; media_dims[i].media; i++) {
		if (!strcmp(media_dims[i].media, media)) {
			*w = media_dims[i].width;
			*h = media_dims[i].height;
			return true;
		}
	}

	return false;
}

/*
 * Printable area.
 *
 * The shape and the arithmetic here are reproduced from a live TouchPad rather
 * than invented, because the enyo render path feeds these pixel counts straight
 * into a canvas and an off-by-a-few would show up as clipped output. Two
 * captures pin it down, both US_Letter at 300 dpi with 0.125in hardware
 * margins:
 *
 *   insets 0.5  -> rawWidth 2475 rawHeight 3225, width 2251 height 3001
 *   insets 0.25 ->                               width 2401 height 3151
 *
 * raw* is the full sheet less the hardware margins, with no rounding bias:
 *   (8.5 - 0.25) * 300 = 2475        (11 - 0.25) * 300 = 3225
 *
 * width/height then subtract only the part of the inset that exceeds the
 * hardware margin, and add one - the original counts the imageable region
 * inclusively:
 *   2475 - (0.375 + 0.375) * 300 + 1 = 2251
 *   2475 - (0.125 + 0.125) * 300 + 1 = 2401
 *
 * The +1 is empirical. It is preserved deliberately: matching the original
 * exactly costs nothing, and a client that sized a buffer from these numbers
 * on real hardware would notice if we changed it.
 */
bool job_compute_area(const char *media_size, const struct print_params *p,
                      int pixel_units, struct printable_area *out, int *err)
{
	double w, h;
	double top, left, right, bottom;

	if (!media_lookup(media_size, &w, &h)) {
		*err = PM_ERR_BAD_MEDIA_SIZE;
		return false;
	}

	if (pixel_units <= 0)
		pixel_units = 300;

	memset(out, 0, sizeof(*out));

	out->pixel_units = pixel_units;
	out->page_width = w;
	out->page_height = h;
	out->page_top_margin = DEFAULT_MARGIN_INCHES;
	out->page_left_margin = DEFAULT_MARGIN_INCHES;
	out->page_right_margin = DEFAULT_MARGIN_INCHES;
	out->page_bottom_margin = DEFAULT_MARGIN_INCHES;

	out->raw_width = (int) lround((w - out->page_left_margin -
	                               out->page_right_margin) * pixel_units);
	out->raw_height = (int) lround((h - out->page_top_margin -
	                                out->page_bottom_margin) * pixel_units);

	/*
	 * Borderless prints to the sheet edge, so the imageable area is the
	 * whole sheet and insets do not apply - the caller has already
	 * rejected the combination of borderless and explicit insets with
	 * -602, so there is nothing to reconcile here.
	 */
	if (p->borderless) {
		out->page_top_margin = 0.0;
		out->page_left_margin = 0.0;
		out->page_right_margin = 0.0;
		out->page_bottom_margin = 0.0;
		out->raw_width = (int) lround(w * pixel_units);
		out->raw_height = (int) lround(h * pixel_units);
		out->width = out->raw_width;
		out->height = out->raw_height;

		return true;
	}

	top = p->top_inset;
	left = p->left_inset;
	right = p->right_inset;
	bottom = p->bottom_inset;

	/*
	 * An inset smaller than the hardware margin cannot be honoured. The
	 * original silently clamped and flagged it with paramsAdjusted rather
	 * than failing, which is the friendlier behaviour for a UI that offers
	 * a margin slider.
	 */
	if (top < out->page_top_margin) {
		top = out->page_top_margin;
		out->params_adjusted = true;
	}
	if (left < out->page_left_margin) {
		left = out->page_left_margin;
		out->params_adjusted = true;
	}
	if (right < out->page_right_margin) {
		right = out->page_right_margin;
		out->params_adjusted = true;
	}
	if (bottom < out->page_bottom_margin) {
		bottom = out->page_bottom_margin;
		out->params_adjusted = true;
	}

	out->width = out->raw_width
	           - (int) lround(((left - out->page_left_margin) +
	                           (right - out->page_right_margin)) * pixel_units)
	           + 1;
	out->height = out->raw_height
	            - (int) lround(((top - out->page_top_margin) +
	                            (bottom - out->page_bottom_margin)) * pixel_units)
	            + 1;

	if (out->width <= 0 || out->height <= 0) {
		*err = PM_ERR_INSET_OUTSIDE_AREA;
		return false;
	}

	return true;
}

static void job_free(gpointer data)
{
	struct print_job *job = data;
	GList *l;

	if (!job)
		return;

	/* Our own scratch files go; files the client supplied are not ours. */
	for (l = job->temp_files; l; l = l->next) {
		if (unlink((const char *) l->data) != 0 && errno != ENOENT)
			g_debug("Could not unlink %s: %s",
			        (const char *) l->data, g_strerror(errno));
	}

	if (job->temp_dir) {
		if (rmdir(job->temp_dir) != 0 && errno != ENOENT)
			g_debug("Could not remove %s: %s", job->temp_dir,
			        g_strerror(errno));
	}

	g_list_free_full(job->temp_files, g_free);
	g_list_free_full(job->files, g_free);
	g_free(job->printer_id);
	g_free(job->description);
	g_free(job->app_name);
	g_free(job->render_result_text);
	g_free(job->last_printer_state);
	g_free(job->temp_dir);
	print_params_clear(&job->params);
	g_free(job);
}

struct job_manager *job_manager_new(const char *spool_root)
{
	struct job_manager *jm;

	jm = g_new0(struct job_manager, 1);
	jm->spool_root = g_strdup(spool_root);
	jm->jobs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
	                                 job_free);
	/* The daemon handed out jobID 1 for the first job of a run. */
	jm->next_job_id = 1;

	if (g_mkdir_with_parents(spool_root, 0700) != 0)
		g_warning("Could not create spool root %s: %s", spool_root,
		          g_strerror(errno));

	return jm;
}

void job_manager_free(struct job_manager *jm)
{
	if (!jm)
		return;

	g_hash_table_destroy(jm->jobs);
	g_free(jm->spool_root);
	g_free(jm);
}

struct print_job *job_manager_open(struct job_manager *jm,
                                   const char *printer_id,
                                   const char *description,
                                   const char *app_name)
{
	struct print_job *job;

	job = g_new0(struct print_job, 1);
	job->job_id = jm->next_job_id++;
	job->cups_job_id = -1;
	job->printer_id = g_strdup(printer_id);
	job->description = g_strdup(description);
	job->app_name = g_strdup(app_name);
	job->phase = JOB_PHASE_OPEN;
	job->current_page = 0;
	job->total_pages = -1;
	job->queued_at = g_get_monotonic_time();
	job->temp_dir = g_strdup_printf("%s/job-%d", jm->spool_root,
	                                job->job_id);

	print_params_init(&job->params);

	if (g_mkdir_with_parents(job->temp_dir, 0700) != 0) {
		g_warning("Could not create job dir %s: %s", job->temp_dir,
		          g_strerror(errno));
		g_clear_pointer(&job->temp_dir, g_free);
	}

	g_hash_table_insert(jm->jobs, GINT_TO_POINTER(job->job_id), job);

	return job;
}

struct print_job *job_manager_find(struct job_manager *jm, int job_id)
{
	return g_hash_table_lookup(jm->jobs, GINT_TO_POINTER(job_id));
}

GList *job_manager_list(struct job_manager *jm)
{
	return g_hash_table_get_values(jm->jobs);
}

void job_manager_remove(struct job_manager *jm, int job_id)
{
	g_hash_table_remove(jm->jobs, GINT_TO_POINTER(job_id));
}

char *job_manager_new_temp_file(struct job_manager *jm, struct print_job *job,
                                const char *extension, gint64 size, int *err)
{
	char *path;
	FILE *fp;

	(void) jm;

	if (!extension || !*extension) {
		*err = PM_ERR_EMPTY_FILE_EXTENSION;
		return NULL;
	}

	if (!job->temp_dir) {
		*err = PM_ERR_NO_FILECACHE_PATH;
		return NULL;
	}

	path = g_strdup_printf("%s/page-%03u.%s", job->temp_dir,
	                       g_list_length(job->temp_files) + 1, extension);

	fp = fopen(path, "wb");
	if (!fp) {
		g_warning("Could not create temp file %s: %s", path,
		          g_strerror(errno));
		g_free(path);
		*err = PM_ERR_NO_FILECACHE_PATH;
		return NULL;
	}

	/*
	 * The old API let a caller reserve space up front so the file cache
	 * could refuse early if it would not fit. There is no cache any more,
	 * but preallocating still turns a late ENOSPC during rendering into an
	 * immediate, reportable -233.
	 */
	if (size > 0) {
		if (ftruncate(fileno(fp), (off_t) size) != 0) {
			g_warning("Could not reserve %" G_GINT64_FORMAT
			          " bytes for %s: %s", size, path,
			          g_strerror(errno));
			fclose(fp);
			unlink(path);
			g_free(path);
			*err = PM_ERR_JOB_TEMP_FILE_NO_ROOM;
			return NULL;
		}
	}

	fclose(fp);

	job->temp_files = g_list_append(job->temp_files, g_strdup(path));

	return path;
}

bool job_manager_resize_temp_file(struct print_job *job, const char *path,
                                  gint64 new_size, int *err)
{
	GList *l;
	bool ours = false;

	for (l = job->temp_files; l; l = l->next) {
		if (!strcmp((const char *) l->data, path)) {
			ours = true;
			break;
		}
	}

	if (!ours) {
		*err = PM_ERR_FILE_NOT_IN_ANY_JOB;
		return false;
	}

	if (truncate(path, (off_t) new_size) != 0) {
		g_warning("Could not resize %s: %s", path, g_strerror(errno));
		*err = PM_ERR_TEMP_FILE_TOO_LARGE;
		return false;
	}

	return true;
}

// vim:ts=4:sw=4:noexpandtab
