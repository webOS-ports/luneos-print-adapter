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
#include <stdlib.h>
#include <unistd.h>

#include <cups/cups.h>

#include "cups_backend.h"
#include "print_errors.h"

/*
 * cups.h defines CUPS_FORMAT_* for the MIME *values* but has no constant for
 * the option name itself. cupsCheckDestSupported() appends "-supported", so
 * this queries document-format-supported.
 */
#define PRINT_OPTION_DOCUMENT_FORMAT	"document-format"

/*
 * How long to wait when opening a connection to a printer.
 *
 * Deliberately short. These calls are synchronous and run on the LS2 main
 * loop, so every millisecond here is a millisecond the whole service is
 * unresponsive. The original daemon dodged this by running the printers
 * methods through deferred wrappers on a worker thread
 * (_printers_method_get_capabilities_wrapper_deferred_wrapper); doing the same
 * is the proper fix and is not done yet.
 */
#define CONNECT_TIMEOUT_MS	5000

/* Formats we are willing to hand to CUPS without converting first. */
static const char *supported_formats[] = {
	CUPS_FORMAT_PDF,
	CUPS_FORMAT_JPEG,
	"image/png",
	NULL,
};

void cups_printer_free(struct cups_printer *p)
{
	if (!p)
		return;

	g_free(p->printer_id);
	g_free(p->printer_name);
	g_free(p->printer_address);
	g_free(p);
}

void cups_caps_free(struct cups_caps *c)
{
	if (!c)
		return;

	g_list_free_full(c->media_sizes, g_free);
	g_list_free_full(c->media_types, g_free);
	g_list_free_full(c->trays, g_free);
	g_free(c);
}

void cups_job_state_free(struct cups_job_state *s)
{
	if (!s)
		return;

	g_list_free_full(s->reasons, g_free);
	g_free(s);
}

/* Pulls the host out of a device or printer URI for the printerAddress field. */
static char *address_from_uri(const char *uri)
{
	char scheme[32], userpass[256], host[256], resource[1024];
	int port;

	if (!uri)
		return g_strdup("");

	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme),
	                    userpass, sizeof(userpass), host, sizeof(host),
	                    &port, resource, sizeof(resource))
	    < HTTP_URI_STATUS_OK)
		return g_strdup("");

	return g_strdup(host);
}

GList *cups_backend_enumerate(void)
{
	cups_dest_t *dests = NULL;
	int i, n;
	GList *out = NULL;

	/*
	 * cupsGetDests2 with a NULL connection returns both the configured
	 * queues and, because CUPS does its own mDNS browsing, the driverless
	 * printers on the network. That is exactly the union the old
	 * printers/list subscription produced from com.palm.zeroconf.
	 */
	n = cupsGetDests2(CUPS_HTTP_DEFAULT, &dests);

	for (i = 0; i < n; i++) {
		cups_dest_t *d = dests + i;
		struct cups_printer *p;
		const char *info, *uri, *type_str;
		unsigned type = 0;

		p = g_new0(struct cups_printer, 1);
		p->printer_id = g_strdup(d->name);

		info = cupsGetOption("printer-info", d->num_options, d->options);
		p->printer_name = g_strdup(info && *info ? info : d->name);

		uri = cupsGetOption("device-uri", d->num_options, d->options);
		if (!uri)
			uri = cupsGetOption("printer-uri-supported",
			                    d->num_options, d->options);
		p->printer_address = address_from_uri(uri);

		type_str = cupsGetOption("printer-type", d->num_options,
		                         d->options);
		if (type_str)
			type = (unsigned) strtoul(type_str, NULL, 10);

		p->is_discovered = (type & CUPS_PRINTER_DISCOVERED) != 0;
		p->supports_ipp = true;

		out = g_list_prepend(out, p);
	}

	cupsFreeDests(n, dests);

	return g_list_reverse(out);
}

static bool option_supports(http_t *http, cups_dest_t *dest,
                            cups_dinfo_t *info, const char *attr,
                            const char *value)
{
	return cupsCheckDestSupported(http, dest, info, attr, value) != 0;
}

static GList *collect_supported(http_t *http, cups_dest_t *dest,
                                cups_dinfo_t *info, const char *attr)
{
	ipp_attribute_t *values;
	GList *out = NULL;
	int i, count;

	values = cupsFindDestSupported(http, dest, info, attr);
	if (!values)
		return NULL;

	count = ippGetCount(values);
	for (i = 0; i < count; i++) {
		const char *v = ippGetString(values, i, NULL);

		if (v)
			out = g_list_prepend(out, g_strdup(v));
	}

	return g_list_reverse(out);
}

struct cups_caps *cups_backend_get_caps(const char *printer_id, int *err)
{
	cups_dest_t *dest;
	cups_dinfo_t *info;
	struct cups_caps *caps;
	ipp_attribute_t *attr;
	http_t *http;
	char resource[1024];
	GList *l;

	dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, printer_id, NULL);
	if (!dest) {
		*err = PM_ERR_PRINTER_UNKNOWN_ID;
		return NULL;
	}

	/*
	 * Ask the printer, not the scheduler.
	 *
	 * cupsCopyDestInfo(CUPS_HTTP_DEFAULT, ...) answers out of whatever
	 * cupsd has cached. For a driverless queue that can be nothing at all -
	 * observed on a Lexmark MC2425adw whose "lpadmin -m everywhere" timed
	 * out before it could fetch attributes, leaving a queue with no PPD.
	 * The capability probes below then all came back false/empty while
	 * still reporting success, which is worse than an error because the UI
	 * would offer a printer with no paper sizes.
	 *
	 * cupsConnectDest() opens a connection to the destination itself, so
	 * the attributes come from the device. Fall back to the scheduler if
	 * the printer will not answer - a local queue with a real PPD is still
	 * perfectly serviceable that way.
	 */
	http = cupsConnectDest(dest, CUPS_DEST_FLAGS_NONE, CONNECT_TIMEOUT_MS,
	                       NULL, resource, sizeof(resource), NULL, NULL);

	info = cupsCopyDestInfo(http ? http : CUPS_HTTP_DEFAULT, dest);
	if (!info) {
		if (http)
			httpClose(http);
		cupsFreeDests(1, dest);
		*err = PM_ERR_COMM_GET_CAPS_FAILED;
		return NULL;
	}

	caps = g_new0(struct cups_caps, 1);

	caps->media_sizes = collect_supported(http, dest, info, CUPS_MEDIA);
	caps->media_types = collect_supported(http, dest, info, CUPS_MEDIA_TYPE);
	caps->trays = collect_supported(http, dest, info, CUPS_MEDIA_SOURCE);

	caps->can_duplex = option_supports(http, dest, info, CUPS_SIDES,
	                                   CUPS_SIDES_TWO_SIDED_PORTRAIT);

	caps->quality_draft = option_supports(http, dest, info, CUPS_PRINT_QUALITY,
	                                      CUPS_PRINT_QUALITY_DRAFT);
	caps->quality_normal = option_supports(http, dest, info, CUPS_PRINT_QUALITY,
	                                       CUPS_PRINT_QUALITY_NORMAL);
	caps->quality_high = option_supports(http, dest, info, CUPS_PRINT_QUALITY,
	                                     CUPS_PRINT_QUALITY_HIGH);

	caps->has_color = option_supports(http, dest, info, CUPS_PRINT_COLOR_MODE,
	                                  CUPS_PRINT_COLOR_MODE_COLOR);
	caps->can_grayscale = option_supports(http, dest, info,
	                                      CUPS_PRINT_COLOR_MODE,
	                                      CUPS_PRINT_COLOR_MODE_MONOCHROME);

	/* A photo tray is just a media-source the printer happens to call photo. */
	for (l = caps->trays; l; l = l->next) {
		if (!strcmp((const char *) l->data, "photo")) {
			caps->has_photo_tray = true;
			break;
		}
	}

	/*
	 * Borderless is advertised as a zero bottom margin. Checking the
	 * margin attribute is more reliable than looking for media names with
	 * "borderless" in them, which is a vendor convention rather than a
	 * standard.
	 */
	attr = cupsFindDestSupported(http, dest, info,
	                             "media-bottom-margin-supported");
	if (attr) {
		int i, count = ippGetCount(attr);

		for (i = 0; i < count; i++) {
			if (ippGetInteger(attr, i) == 0) {
				caps->can_borderless = true;
				break;
			}
		}
	}

	attr = cupsFindDestSupported(http, dest, info,
	                             "printer-resolution-supported");
	if (attr) {
		int i, count = ippGetCount(attr);

		for (i = 0; i < count; i++) {
			ipp_res_t units;
			int xres, yres;

			xres = ippGetResolution(attr, i, &yres, &units);
			if (units == IPP_RES_PER_INCH && xres > caps->max_resolution)
				caps->max_resolution = xres;
		}
	}

	/* Cancel-Job is REQUIRED of every IPP printer (RFC 8011 s4.3.8), so
	 * there is nothing to probe - any printer we can reach can cancel. */
	caps->can_cancel = true;

	/*
	 * "Supported" for our purposes means the printer takes at least one
	 * format we can hand over without a PDL of our own. This is the modern
	 * replacement for the old -238 "no pcl3gui or pcl5" check.
	 */
	{
		int i;

		for (i = 0; supported_formats[i]; i++) {
			if (option_supports(http, dest, info,
			                    PRINT_OPTION_DOCUMENT_FORMAT,
			                    supported_formats[i])) {
				caps->is_supported = true;
				break;
			}
		}
	}

	cupsFreeDestInfo(info);
	if (http)
		httpClose(http);
	cupsFreeDests(1, dest);

	return caps;
}

static int add_options(cups_option_t **options,
                       const struct cups_job_params *p)
{
	int num = 0;
	char buf[64];

	if (p->media)
		num = cupsAddOption(CUPS_MEDIA, p->media, num, options);
	if (p->media_type)
		num = cupsAddOption(CUPS_MEDIA_TYPE, p->media_type, num, options);
	if (p->media_source)
		num = cupsAddOption(CUPS_MEDIA_SOURCE, p->media_source, num,
		                    options);
	if (p->sides)
		num = cupsAddOption(CUPS_SIDES, p->sides, num, options);
	if (p->color_mode)
		num = cupsAddOption(CUPS_PRINT_COLOR_MODE, p->color_mode, num,
		                    options);
	if (p->print_scaling)
		num = cupsAddOption("print-scaling", p->print_scaling, num,
		                    options);

	if (p->quality) {
		snprintf(buf, sizeof(buf), "%d", p->quality);
		num = cupsAddOption(CUPS_PRINT_QUALITY, buf, num, options);
	}

	if (p->copies > 0) {
		snprintf(buf, sizeof(buf), "%d", p->copies);
		num = cupsAddOption(CUPS_COPIES, buf, num, options);
	}

	if (p->resolution > 0) {
		snprintf(buf, sizeof(buf), "%ddpi", p->resolution);
		num = cupsAddOption("printer-resolution", buf, num, options);
	}

	/*
	 * Margins go through media-col. Borderless is simply all four at zero,
	 * which is why the caller must not set both borderless and insets -
	 * the original rejected that combination with -602 and so do we,
	 * before we ever get here.
	 */
	if (p->borderless) {
		num = cupsAddOption("media-top-margin", "0", num, options);
		num = cupsAddOption("media-left-margin", "0", num, options);
		num = cupsAddOption("media-right-margin", "0", num, options);
		num = cupsAddOption("media-bottom-margin", "0", num, options);
	} else if (p->margins_valid) {
		snprintf(buf, sizeof(buf), "%d", p->top_margin);
		num = cupsAddOption("media-top-margin", buf, num, options);
		snprintf(buf, sizeof(buf), "%d", p->left_margin);
		num = cupsAddOption("media-left-margin", buf, num, options);
		snprintf(buf, sizeof(buf), "%d", p->right_margin);
		num = cupsAddOption("media-right-margin", buf, num, options);
		snprintf(buf, sizeof(buf), "%d", p->bottom_margin);
		num = cupsAddOption("media-bottom-margin", buf, num, options);
	}

	return num;
}

int cups_backend_submit(const char *printer_id, const char *title,
                        GList *files, const struct cups_job_params *params,
                        int *err)
{
	cups_option_t *options = NULL;
	int num_options;
	int job_id;
	GList *l;
	guint n_files, idx;

	if (!files) {
		*err = PM_ERR_NO_FILES_TO_PRINT;
		return -1;
	}

	num_options = add_options(&options, params);

	job_id = cupsCreateJob(CUPS_HTTP_DEFAULT, printer_id,
	                       title ? title : "LuneOS print job",
	                       num_options, options);

	if (job_id <= 0) {
		g_warning("cupsCreateJob failed for %s: %s", printer_id,
		          cupsLastErrorString());
		cupsFreeOptions(num_options, options);
		*err = PM_ERR_JOB_START_FAILED;
		return -1;
	}

	n_files = g_list_length(files);

	for (l = files, idx = 0; l; l = l->next, idx++) {
		const char *path = l->data;
		const char *format;
		char chunk[16384];
		FILE *fp;
		size_t got;
		bool last = (idx + 1 == n_files);

		format = cups_backend_detect_format(path);
		if (!format) {
			g_warning("Refusing to print %s: unsupported format",
			          path);
			cupsCancelJob2(CUPS_HTTP_DEFAULT, printer_id, job_id, 0);
			cupsFreeOptions(num_options, options);
			*err = PM_ERR_UNSUPPORTED_MIME_TYPE;
			return -1;
		}

		fp = fopen(path, "rb");
		if (!fp) {
			cupsCancelJob2(CUPS_HTTP_DEFAULT, printer_id, job_id, 0);
			cupsFreeOptions(num_options, options);
			*err = PM_ERR_FILE_DOES_NOT_EXIST;
			return -1;
		}

		if (cupsStartDocument(CUPS_HTTP_DEFAULT, printer_id, job_id,
		                      path, format, last ? 1 : 0)
		    != HTTP_STATUS_CONTINUE) {
			fclose(fp);
			cupsCancelJob2(CUPS_HTTP_DEFAULT, printer_id, job_id, 0);
			cupsFreeOptions(num_options, options);
			*err = PM_ERR_JOB_ADD_PAGE_FAILED;
			return -1;
		}

		while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
			if (cupsWriteRequestData(CUPS_HTTP_DEFAULT, chunk, got)
			    != HTTP_STATUS_CONTINUE) {
				fclose(fp);
				cupsFinishDocument(CUPS_HTTP_DEFAULT,
				                   printer_id);
				cupsCancelJob2(CUPS_HTTP_DEFAULT, printer_id,
				               job_id, 0);
				cupsFreeOptions(num_options, options);
				*err = PM_ERR_JOB_ADD_PAGE_FAILED;
				return -1;
			}
		}

		fclose(fp);

		if (cupsFinishDocument(CUPS_HTTP_DEFAULT, printer_id)
		    != IPP_STATUS_OK) {
			ipp_status_t st = cupsLastError();

			g_warning("cupsFinishDocument failed: %s",
			          cupsLastErrorString());
			cupsCancelJob2(CUPS_HTTP_DEFAULT, printer_id, job_id, 0);
			cupsFreeOptions(num_options, options);

			/*
			 * A queue with no filter chain rejects anything it
			 * cannot print natively - printing a PDF to a
			 * driverless queue on an image without cups-filters
			 * gives exactly this. Reporting it as a generic page
			 * failure sends the user hunting in the wrong place,
			 * so surface it as the format error it is.
			 */
			if (st == IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED)
				*err = PM_ERR_UNSUPPORTED_MIME_TYPE;
			else
				*err = PM_ERR_JOB_ADD_PAGE_FAILED;

			return -1;
		}
	}

	cupsFreeOptions(num_options, options);

	return job_id;
}

bool cups_backend_cancel(const char *printer_id, int cups_job_id)
{
	return cupsCancelJob2(CUPS_HTTP_DEFAULT, printer_id, cups_job_id, 0)
	       == IPP_STATUS_OK;
}

struct cups_job_state *cups_backend_get_job_state(const char *printer_id,
                                                  int cups_job_id)
{
	cups_job_t *jobs = NULL;
	int i, n;
	struct cups_job_state *state = NULL;

	n = cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, printer_id, 0,
	                 CUPS_WHICHJOBS_ALL);

	for (i = 0; i < n; i++) {
		if (jobs[i].id != cups_job_id)
			continue;

		state = g_new0(struct cups_job_state, 1);
		state->ipp_state = (int) jobs[i].state;
		state->current_page = -1;
		state->total_pages = -1;
		break;
	}

	cupsFreeJobs(n, jobs);

	if (!state)
		return NULL;

	/*
	 * printer-state-reasons is a printer attribute, not a job one, so it
	 * has to be fetched separately. It is what turns a bare
	 * processing-stopped into something a user can act on ("out of paper").
	 */
	{
		cups_dest_t *dest;

		dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, printer_id, NULL);
		if (dest) {
			const char *reasons, *markers;

			reasons = cupsGetOption("printer-state-reasons",
			                        dest->num_options,
			                        dest->options);
			if (reasons) {
				gchar **parts = g_strsplit(reasons, ",", -1);
				int k;

				for (k = 0; parts[k]; k++)
					state->reasons =
						g_list_prepend(state->reasons,
						               g_strdup(g_strstrip(parts[k])));

				g_strfreev(parts);
				state->reasons = g_list_reverse(state->reasons);
			}

			markers = cupsGetOption("marker-types",
			                        dest->num_options,
			                        dest->options);
			if (markers && strstr(markers, "toner"))
				state->is_toner = true;

			cupsFreeDests(1, dest);
		}
	}

	return state;
}

const char *cups_backend_detect_format(const char *path)
{
	unsigned char head[16];
	FILE *fp;
	size_t got;

	fp = fopen(path, "rb");
	if (!fp)
		return NULL;

	got = fread(head, 1, sizeof(head), fp);
	fclose(fp);

	if (got < 4)
		return NULL;

	if (!memcmp(head, "%PDF", 4))
		return CUPS_FORMAT_PDF;

	/* JPEG SOI followed by an APPn or JFIF marker */
	if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
		return CUPS_FORMAT_JPEG;

	if (got >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8))
		return "image/png";

	return NULL;
}

// vim:ts=4:sw=4:noexpandtab
