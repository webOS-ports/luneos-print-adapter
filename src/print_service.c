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

/*
 * com.palm.printmgr / com.webos.service.print
 *
 * A translation shim. The webOS printing API is preserved exactly - the same
 * two categories, the same 23 methods, the same keyword vocabulary, the same
 * numeric error codes - but underneath it is CUPS and IPP rather than HP's
 * wprint PDL blobs, which were ARM-only, closed source, and the reason the
 * original only really worked with HP hardware.
 *
 * The API surface was recovered from the shipped printmgrd binary (which
 * embeds a JSON schema per method as string constants) and then confirmed
 * against a live TouchPad. See ../../print-notes.md for the full derivation.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include <glib.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>

#include "print_service.h"
#include "luna_service_utils.h"
#include "print_errors.h"
#include "param_map.h"
#include "printer_db.h"
#include "cups_backend.h"
#include "job_manager.h"

#define PRINT_SERVICE_LEGACY_NAME	"com.palm.printmgr"
#define PRINT_SERVICE_NAME		"com.webos.service.print"

/*
 * The Settings app's Print Manager page speaks a different, simpler API than
 * the webOS one, under its own bus name. Both are served: the legacy surface
 * is what the enyo print dialog and any ported app calls, and this one is what
 * the LuneOS Settings page was written against. They share all the machinery
 * underneath - same CUPS backend, same printer database, same notion of which
 * printer is the default - and differ only in the shape of the JSON.
 */
#define PORTS_SERVICE_NAME		"org.webosports.service.print"

#define CAT_ROOT			"/"
#define METHOD_LIST_PRINTERS		"listPrinters"
#define METHOD_LIST_JOBS		"listJobs"

#define PRINTER_DB_PATH		"/var/preferences/com.palm.printmgr/printers.conf"
#define SPOOL_ROOT		"/var/spool/luneos-print"

/* How often we poll CUPS for job state while any job is in flight. */
#define JOB_POLL_INTERVAL_MS	1000

/*
 * Subscription addressing.
 *
 * LSSubscriptionPost() takes the category and the method as two arguments and
 * joins them itself to form the key LSSubscriptionProcess() registered the
 * subscriber under. Passing the joined path as both - which is easy to do and
 * compiles fine - yields a key nothing is subscribed to, so every post is
 * silently dropped and a client sees only its initial ack. Keep them split.
 */
#define CAT_PRINTERS		"/printers"
#define CAT_JOBS		"/jobs"
#define METHOD_LIST		"list"
#define METHOD_GET_STATUS	"getStatus"
#define METHOD_GET_RENDER_STATUS "getRenderStatus"

extern GMainLoop *event_loop;

struct print_service {
	LSHandle *handle;
	LSHandle *ports_handle;		/* org.webosports.service.print */
	struct printer_db *db;
	struct job_manager *jobs;
	GList *known_printers;		/* struct cups_printer*, last browse */
	guint browse_source;
	guint poll_source;
	char *ssid;			/* current network, for printer scoping */
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void reply_err(LSHandle *h, LSMessage *m, int code)
{
	luna_service_message_reply_error_code(h, m, code);
}

/*
 * Every method takes an optional "subscribe". The original replied with
 * subscribed:true/false alongside the payload, so mirror that.
 */
static void put_subscribed(jvalue_ref obj, bool subscribed)
{
	jobject_put(obj, J_CSTR_TO_JVAL("subscribed"),
	            jboolean_create(subscribed));
}

static void put_ok(jvalue_ref obj)
{
	jobject_put(obj, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
}

static jvalue_ref printer_to_json(const struct cups_printer *p)
{
	jvalue_ref o = jobject_create();

	jobject_put(o, J_CSTR_TO_JVAL("printerID"),
	            jstring_create(p->printer_id));
	jobject_put(o, J_CSTR_TO_JVAL("printerName"),
	            jstring_create(p->printer_name));
	jobject_put(o, J_CSTR_TO_JVAL("printerAddress"),
	            jstring_create(p->printer_address));

	return o;
}

static struct cups_printer *find_known(struct print_service *s,
                                       const char *printer_id)
{
	GList *l;

	for (l = s->known_printers; l; l = l->next) {
		struct cups_printer *p = l->data;

		if (!strcmp(p->printer_id, printer_id))
			return p;
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* discovery                                                          */
/* ------------------------------------------------------------------ */

static void post_printer_event(struct print_service *s,
                               const struct cups_printer *p,
                               const char *event_type)
{
	jvalue_ref o = printer_to_json(p);

	jobject_put(o, J_CSTR_TO_JVAL("eventType"), jstring_create(event_type));
	/*
	 * The UI filters on this to keep manually added printers out of the
	 * "auto discovered" list. CUPS tells us whether a dest came from mDNS
	 * or is a locally configured queue, which is the same distinction.
	 */
	jobject_put(o, J_CSTR_TO_JVAL("source"),
	            jstring_create(p->is_discovered ? "fromZeroconf"
	                                            : "fromManualAdd"));

	luna_service_post_subscription(s->handle, CAT_PRINTERS, METHOD_LIST, o);
	j_release(&o);
}

static void ports_post_printers(struct print_service *s);
static void ports_post_jobs(struct print_service *s);

static gint printer_cmp_id(gconstpointer a, gconstpointer b)
{
	const struct cups_printer *pa = a;

	return strcmp(pa->printer_id, (const char *) b);
}

/*
 * Re-browse and emit Add/Rmv for the difference. Clients treat printers/list
 * as a live feed, so the first browse after a subscription looks like a burst
 * of Adds, exactly as the zeroconf-backed original did.
 */
static gboolean browse_printers(gpointer data)
{
	struct print_service *s = data;
	GList *current, *l;

	current = cups_backend_enumerate();

	for (l = current; l; l = l->next) {
		struct cups_printer *p = l->data;

		if (!g_list_find_custom(s->known_printers, p->printer_id,
		                        printer_cmp_id))
			post_printer_event(s, p, "Add");
	}

	for (l = s->known_printers; l; l = l->next) {
		struct cups_printer *p = l->data;

		if (!g_list_find_custom(current, p->printer_id, printer_cmp_id))
			post_printer_event(s, p, "Rmv");
	}

	g_list_free_full(s->known_printers, (GDestroyNotify) cups_printer_free);
	s->known_printers = current;

	ports_post_printers(s);

	return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* /printers                                                          */
/* ------------------------------------------------------------------ */

static bool printers_list(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	bool subscribed;
	GList *l;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	subscribed = luna_service_check_for_subscription_and_process(h, m);

	reply = jobject_create();
	put_ok(reply);
	put_subscribed(reply, subscribed);
	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	/*
	 * Replay what we already know to the new subscriber. The original did
	 * the same - a subscriber that arrives after discovery has settled
	 * still needs to learn about the printers already on the network.
	 */
	if (subscribed) {
		for (l = s->known_printers; l; l = l->next)
			post_printer_event(s, l->data, "Add");
	}

	j_release(&parsed);

	return true;
}

static bool printers_list_added(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply, arr;
	bool ssid_all = false;
	GList *list, *l;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	luna_get_bool(parsed, "ssidAll", &ssid_all);

	list = printer_db_list(s->db, ssid_all ? NULL : s->ssid);

	arr = jarray_create(NULL);
	for (l = list; l; l = l->next) {
		struct printer_record *rec = l->data;
		jvalue_ref o = jobject_create();

		jobject_put(o, J_CSTR_TO_JVAL("printerID"),
		            jstring_create(rec->printer_id));
		jobject_put(o, J_CSTR_TO_JVAL("printerName"),
		            jstring_create(rec->printer_name ?
		                           rec->printer_name : ""));
		jobject_put(o, J_CSTR_TO_JVAL("printerAddress"),
		            jstring_create(rec->printer_address ?
		                           rec->printer_address : ""));
		jobject_put(o, J_CSTR_TO_JVAL("ssid"),
		            jstring_create(rec->ssid ? rec->ssid : ""));

		jarray_append(arr, o);
	}
	g_list_free_full(list, (GDestroyNotify) printer_record_free);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("addList"), arr);
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static bool printers_list_active(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref reply, arr;
	GList *l;

	/*
	 * The device returns every printer it currently knows about with a
	 * handful of transport flags, not only printers with live jobs -
	 * confirmed by capture. Reproduce that, mapping the flags onto what
	 * IPP can actually tell us.
	 */
	arr = jarray_create(NULL);
	for (l = s->known_printers; l; l = l->next) {
		struct cups_printer *p = l->data;
		jvalue_ref o = printer_to_json(p);

		jobject_put(o, J_CSTR_TO_JVAL("supportsIpp"),
		            jboolean_create(p->supports_ipp));
		jobject_put(o, J_CSTR_TO_JVAL("supportsPdl"),
		            jboolean_create(false));
		jobject_put(o, J_CSTR_TO_JVAL("source"),
		            jstring_create(p->is_discovered ? "fromZeroconf"
		                                            : "fromManualAdd"));
		jobject_put(o, J_CSTR_TO_JVAL("unusableAddress"),
		            jboolean_create(false));

		jarray_append(arr, o);
	}

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("activePrinters"), arr);
	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	return true;
}

static bool printers_get_current(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	const char *current;
	struct cups_printer *p;
	jvalue_ref reply;

	current = printer_db_get_current(s->db);

	reply = jobject_create();
	put_ok(reply);

	p = current && *current ? find_known(s, current) : NULL;

	if (p) {
		jobject_put(reply, J_CSTR_TO_JVAL("printerID"),
		            jstring_create(p->printer_id));
		jobject_put(reply, J_CSTR_TO_JVAL("printerName"),
		            jstring_create(p->printer_name));
		jobject_put(reply, J_CSTR_TO_JVAL("printerAddress"),
		            jstring_create(p->printer_address));
	} else {
		/*
		 * With nothing selected the daemon returned empty strings
		 * rather than omitting the keys or failing, and the UI relies
		 * on that to decide whether to show the picker.
		 */
		jobject_put(reply, J_CSTR_TO_JVAL("printerID"),
		            jstring_create(current ? current : ""));
		jobject_put(reply, J_CSTR_TO_JVAL("printerName"),
		            jstring_create(""));
		jobject_put(reply, J_CSTR_TO_JVAL("printerAddress"),
		            jstring_create(""));
	}

	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	return true;
}

static bool printers_set_current(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	const char *printer_id;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerID", &printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!printer_db_set_current(s->db, printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_SET_CURRENT_PRINTER_FAILED);
		return true;
	}

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool printers_add(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	const char *printer_id, *printer_address, *printer_name = NULL;
	int err = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerID", &printer_id) ||
	    !luna_get_string(parsed, "printerAddress", &printer_address)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	luna_get_string(parsed, "printerName", &printer_name);

	/*
	 * The UI's manual-add flow puts the typed IP in both printerID and
	 * printerAddress, so validating the address covers both. g_hostname
	 * would accept a name too, but the original insisted on an IP and
	 * returned -206 otherwise; keep that contract.
	 */
	if (!g_hostname_is_ip_address(printer_address)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_PRINTER_IP_NOT_VALID);
		return true;
	}

	if (!printer_db_add(s->db, printer_id, printer_name, printer_address,
	                    s->ssid, &err)) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool printers_delete(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	const char *printer_id, *ssid = NULL;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerID", &printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	luna_get_string(parsed, "ssid", &ssid);

	if (!printer_db_delete(s->db, printer_id, ssid)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_PRINTER_UNKNOWN_ID);
		return true;
	}

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static void append_mapped_list(jvalue_ref arr, GList *values,
                               const char *(*map)(const char *))
{
	GList *l;
	GHashTable *seen;

	seen = g_hash_table_new(g_str_hash, g_str_equal);

	for (l = values; l; l = l->next) {
		const char *webos = map((const char *) l->data);

		/* Drop anything with no webOS keyword, and de-duplicate: two
		 * IPP names can fold onto one webOS keyword (5x7 does). */
		if (!webos || g_hash_table_contains(seen, webos))
			continue;

		g_hash_table_add(seen, (gpointer) webos);
		jarray_append(arr, jstring_create(webos));
	}

	g_hash_table_destroy(seen);
}

static bool printers_get_capabilities(LSHandle *h, LSMessage *m, void *ctx)
{
	jvalue_ref parsed, reply, sizes, types, trays;
	const char *printer_id;
	struct cups_caps *caps;
	int err = 0;

	(void) ctx;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerID", &printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	caps = cups_backend_get_caps(printer_id, &err);
	if (!caps) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	sizes = jarray_create(NULL);
	append_mapped_list(sizes, caps->media_sizes, pm_media_pwg_to_webos);

	types = jarray_create(NULL);
	append_mapped_list(types, caps->media_types, pm_mediatype_ipp_to_webos);

	trays = jarray_create(NULL);
	append_mapped_list(trays, caps->trays, pm_tray_ipp_to_webos);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("mediaSize"), sizes);
	jobject_put(reply, J_CSTR_TO_JVAL("mediaType"), types);
	jobject_put(reply, J_CSTR_TO_JVAL("trays"), trays);
	jobject_put(reply, J_CSTR_TO_JVAL("canDuplex"),
	            jboolean_create(caps->can_duplex));
	jobject_put(reply, J_CSTR_TO_JVAL("hasPhotoTray"),
	            jboolean_create(caps->has_photo_tray));
	jobject_put(reply, J_CSTR_TO_JVAL("canPrintBorderless"),
	            jboolean_create(caps->can_borderless));
	jobject_put(reply, J_CSTR_TO_JVAL("canPrintQualityDraft"),
	            jboolean_create(caps->quality_draft));
	jobject_put(reply, J_CSTR_TO_JVAL("canPrintQualityNormal"),
	            jboolean_create(caps->quality_normal));
	jobject_put(reply, J_CSTR_TO_JVAL("canPrintQualityHigh"),
	            jboolean_create(caps->quality_high));
	jobject_put(reply, J_CSTR_TO_JVAL("hasColor"),
	            jboolean_create(caps->has_color));
	/* No IPP equivalent; the original reported it for HP duplexers only. */
	jobject_put(reply, J_CSTR_TO_JVAL("hasFaceDownTray"),
	            jboolean_create(false));
	jobject_put(reply, J_CSTR_TO_JVAL("isSupported"),
	            jboolean_create(caps->is_supported));
	jobject_put(reply, J_CSTR_TO_JVAL("canCancel"),
	            jboolean_create(caps->can_cancel));
	jobject_put(reply, J_CSTR_TO_JVAL("canGrayscale"),
	            jboolean_create(caps->can_grayscale));

	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	cups_caps_free(caps);
	j_release(&parsed);

	return true;
}

/* ------------------------------------------------------------------ */
/* job status                                                         */
/* ------------------------------------------------------------------ */

static jvalue_ref job_status_base(const struct print_job *job)
{
	jvalue_ref o = jobject_create();

	jobject_put(o, J_CSTR_TO_JVAL("jobID"), jnumber_create_i32(job->job_id));
	jobject_put(o, J_CSTR_TO_JVAL("printerID"),
	            jstring_create(job->printer_id ? job->printer_id : ""));
	jobject_put(o, J_CSTR_TO_JVAL("description"),
	            jstring_create(job->description ? job->description : ""));
	jobject_put(o, J_CSTR_TO_JVAL("appName"),
	            jstring_create(job->app_name ? job->app_name : ""));

	return o;
}

static void post_job_state(struct print_service *s, struct print_job *job,
                           const char *printer_state, const char *job_status,
                           GList *blocked_reasons)
{
	jvalue_ref o = job_status_base(job);
	jvalue_ref arr;
	GList *l;

	jobject_put(o, J_CSTR_TO_JVAL("statusId"), jstring_create("JOBSTATE"));
	jobject_put(o, J_CSTR_TO_JVAL("printerState"),
	            jstring_create(printer_state));

	if (job_status)
		jobject_put(o, J_CSTR_TO_JVAL("jobStatus"),
		            jstring_create(job_status));

	if (!strcmp(printer_state, "DONE")) {
		gint64 now = g_get_monotonic_time();

		jobject_put(o, J_CSTR_TO_JVAL("timeActive"),
		            jnumber_create_i32(job->started_at ?
		                (int) ((now - job->started_at) / G_USEC_PER_SEC)
		                : 0));
		jobject_put(o, J_CSTR_TO_JVAL("timeFromQueued"),
		            jnumber_create_i32((int) ((now - job->queued_at) /
		                                      G_USEC_PER_SEC)));
	} else {
		jobject_put(o, J_CSTR_TO_JVAL("currentPage"),
		            jnumber_create_i32(job->current_page));
		jobject_put(o, J_CSTR_TO_JVAL("totalPages"),
		            jnumber_create_i32(job->total_pages));

		arr = jarray_create(NULL);
		for (l = blocked_reasons; l; l = l->next)
			jarray_append(arr, jstring_create((const char *) l->data));
		jobject_put(o, J_CSTR_TO_JVAL("blockedReasons"), arr);
	}

	luna_service_post_subscription(s->handle, CAT_JOBS, METHOD_GET_STATUS, o);
	j_release(&o);
}

static void post_page_info(struct print_service *s, struct print_job *job,
                           const char *status_id, int page)
{
	jvalue_ref o = job_status_base(job);

	jobject_put(o, J_CSTR_TO_JVAL("statusId"), jstring_create(status_id));
	jobject_put(o, J_CSTR_TO_JVAL("pageNum"), jnumber_create_i32(page));
	jobject_put(o, J_CSTR_TO_JVAL("copyNum"), jnumber_create_i32(1));
	jobject_put(o, J_CSTR_TO_JVAL("currentPage"), jnumber_create_i32(page));
	jobject_put(o, J_CSTR_TO_JVAL("totalPages"),
	            jnumber_create_i32(job->total_pages));
	/* The daemon sent this as the *string* "false"/"true", not a boolean. */
	jobject_put(o, J_CSTR_TO_JVAL("pageCorrupted"), jstring_create("false"));
	jobject_put(o, J_CSTR_TO_JVAL("pageTime"), jnumber_create_i32(0));

	luna_service_post_subscription(s->handle, CAT_JOBS, METHOD_GET_STATUS, o);
	j_release(&o);
}

/*
 * Polls CUPS for every submitted job and turns state changes into the webOS
 * status stream. Only runs while something is in flight.
 */
static gboolean poll_jobs(gpointer data)
{
	struct print_service *s = data;
	GList *jobs, *l;
	bool any_active = false;

	jobs = job_manager_list(s->jobs);

	for (l = jobs; l; l = l->next) {
		struct print_job *job = l->data;
		struct cups_job_state *st;
		const char *printer_state, *job_status;
		GList *blocked = NULL, *r;

		if (job->phase != JOB_PHASE_SUBMITTED)
			continue;

		any_active = true;

		st = cups_backend_get_job_state(job->printer_id,
		                                job->cups_job_id);
		if (!st) {
			/*
			 * CUPS forgets completed jobs once they age out of the
			 * history. A submitted job that has vanished finished;
			 * reporting Success beats leaving a client subscribed
			 * forever waiting for a terminal state.
			 */
			job->phase = JOB_PHASE_DONE;
			post_job_state(s, job, "DONE", "Success", NULL);
			continue;
		}

		pm_jobstate_ipp_to_webos(st->ipp_state, &printer_state,
		                         &job_status);

		for (r = st->reasons; r; r = r->next) {
			const char *mapped;

			mapped = pm_reason_ipp_to_webos((const char *) r->data,
			                                st->is_toner);
			if (mapped)
				blocked = g_list_append(blocked,
				                        (gpointer) mapped);
		}

		if (!job->last_printer_state ||
		    strcmp(job->last_printer_state, printer_state)) {

			if (!strcmp(printer_state, "RUNNING") &&
			    !job->started_at)
				job->started_at = g_get_monotonic_time();

			post_job_state(s, job, printer_state, job_status,
			               blocked);

			g_free(job->last_printer_state);
			job->last_printer_state = g_strdup(printer_state);

			if (!strcmp(printer_state, "DONE"))
				job->phase = JOB_PHASE_DONE;
		}

		g_list_free(blocked);
		cups_job_state_free(st);
	}

	g_list_free(jobs);

	/* The Settings queue view is driven from CUPS directly, so it needs a
	 * push on every poll rather than only when one of our own jobs moves. */
	ports_post_jobs(s);

	if (!any_active) {
		s->poll_source = 0;
		return G_SOURCE_REMOVE;
	}

	return G_SOURCE_CONTINUE;
}

static void ensure_polling(struct print_service *s)
{
	if (s->poll_source)
		return;

	s->poll_source = g_timeout_add(JOB_POLL_INTERVAL_MS, poll_jobs, s);
}

/* ------------------------------------------------------------------ */
/* /jobs                                                              */
/* ------------------------------------------------------------------ */

static bool jobs_open(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	const char *printer_id, *description, *app_name;
	struct print_job *job;
	bool subscribed;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	/*
	 * All three are required by the embedded schema, and the live daemon
	 * rejects a call missing any of them with -610 before the handler
	 * runs. Enforce the same up front.
	 */
	if (!luna_get_string(parsed, "printerID", &printer_id) ||
	    !luna_get_string(parsed, "description", &description) ||
	    !luna_get_string(parsed, "appName", &app_name)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_open(s->jobs, printer_id, description, app_name);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_JOB_NO_JOB_HANDLES);
		return true;
	}

	subscribed = luna_service_check_for_subscription_and_process(h, m);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("jobID"),
	            jnumber_create_i32(job->job_id));
	put_subscribed(reply, subscribed);
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static void put_params(jvalue_ref o, const struct print_params *p)
{
	jobject_put(o, J_CSTR_TO_JVAL("mediaSize"), jstring_create(p->media_size));
	jobject_put(o, J_CSTR_TO_JVAL("mediaType"), jstring_create(p->media_type));
	jobject_put(o, J_CSTR_TO_JVAL("printQuality"),
	            jstring_create(p->print_quality));
	jobject_put(o, J_CSTR_TO_JVAL("duplex"), jstring_create(p->duplex));
	jobject_put(o, J_CSTR_TO_JVAL("dryTime"), jstring_create(p->dry_time));
	jobject_put(o, J_CSTR_TO_JVAL("color"), jstring_create(p->color));
	jobject_put(o, J_CSTR_TO_JVAL("tray"), jstring_create(p->tray));
	jobject_put(o, J_CSTR_TO_JVAL("numCopies"),
	            jnumber_create_i32(p->num_copies));
	jobject_put(o, J_CSTR_TO_JVAL("borderless"),
	            jboolean_create(p->borderless));
	jobject_put(o, J_CSTR_TO_JVAL("autoRotate"),
	            jboolean_create(p->auto_rotate));
	jobject_put(o, J_CSTR_TO_JVAL("autoScale"),
	            jboolean_create(p->auto_scale));
	jobject_put(o, J_CSTR_TO_JVAL("topInset"),
	            jnumber_create_f64(p->top_inset));
	jobject_put(o, J_CSTR_TO_JVAL("leftInset"),
	            jnumber_create_f64(p->left_inset));
	jobject_put(o, J_CSTR_TO_JVAL("rightInset"),
	            jnumber_create_f64(p->right_inset));
	jobject_put(o, J_CSTR_TO_JVAL("bottomInset"),
	            jnumber_create_f64(p->bottom_inset));
}

static bool jobs_get_current_params(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	struct print_job *job;
	struct printable_area area;
	int job_id, err = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	reply = jobject_create();
	put_ok(reply);
	put_params(reply, &job->params);

	/*
	 * getCurrentPrintParams also reports the sheet and its hardware
	 * margins - but not the pixel geometry, which only appears in
	 * getFinalParamsAndArea. Confirmed by capture.
	 */
	if (job_compute_area(job->params.media_size, &job->params, 300, &area,
	                     &err)) {
		jobject_put(reply, J_CSTR_TO_JVAL("pageWidth"),
		            jnumber_create_f64(area.page_width));
		jobject_put(reply, J_CSTR_TO_JVAL("pageHeight"),
		            jnumber_create_f64(area.page_height));
		jobject_put(reply, J_CSTR_TO_JVAL("pageTopMargin"),
		            jnumber_create_f64(area.page_top_margin));
		jobject_put(reply, J_CSTR_TO_JVAL("pageLeftMargin"),
		            jnumber_create_f64(area.page_left_margin));
		jobject_put(reply, J_CSTR_TO_JVAL("pageRightMargin"),
		            jnumber_create_f64(area.page_right_margin));
		jobject_put(reply, J_CSTR_TO_JVAL("pageBottomMargin"),
		            jnumber_create_f64(area.page_bottom_margin));
	}

	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static bool jobs_edit_params(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	struct print_job *job;
	struct print_params *p;
	const char *str;
	int job_id, n;
	bool b;
	double d;
	bool insets_touched = false;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	/*
	 * Job existence is checked before any of the enum values - the live
	 * daemon returns -617 for editPrintParams on an unknown job even when
	 * the mediaSize is also bogus.
	 */
	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	if (job->phase != JOB_PHASE_OPEN) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_JOB_ALREADY_CLOSED);
		return true;
	}

	p = &job->params;

#define TAKE_ENUM(key, field, validator, errcode)			\
	do {								\
		if (luna_get_string(parsed, key, &str)) {		\
			if (!validator(str)) {				\
				j_release(&parsed);			\
				reply_err(h, m, errcode);		\
				return true;				\
			}						\
			g_free(p->field);				\
			p->field = g_strdup(str);			\
		}							\
	} while (0)

	TAKE_ENUM("mediaSize", media_size, pm_media_is_valid,
	          PM_ERR_BAD_MEDIA_SIZE);
	TAKE_ENUM("mediaType", media_type, pm_mediatype_is_valid,
	          PM_ERR_BAD_MEDIA_TYPE);
	TAKE_ENUM("printQuality", print_quality, pm_quality_is_valid,
	          PM_ERR_BAD_PRINT_QUALITY);
	TAKE_ENUM("duplex", duplex, pm_duplex_is_valid, PM_ERR_BAD_DUPLEX);
	TAKE_ENUM("dryTime", dry_time, pm_drytime_is_valid, PM_ERR_BAD_DRYTIME);
	TAKE_ENUM("color", color, pm_color_is_valid, PM_ERR_BAD_COLOR);
	TAKE_ENUM("tray", tray, pm_tray_is_valid, PM_ERR_BAD_TRAY);

#undef TAKE_ENUM

	if (luna_get_int(parsed, "numCopies", &n)) {
		if (n < 1 || n > 99) {
			j_release(&parsed);
			reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
			return true;
		}
		p->num_copies = n;
	}

	if (luna_get_bool(parsed, "borderless", &b))
		p->borderless = b;
	if (luna_get_bool(parsed, "autoRotate", &b))
		p->auto_rotate = b;
	if (luna_get_bool(parsed, "autoScale", &b))
		p->auto_scale = b;

	if (luna_get_double(parsed, "topInset", &d)) {
		p->top_inset = d;
		insets_touched = true;
	}
	if (luna_get_double(parsed, "leftInset", &d)) {
		p->left_inset = d;
		insets_touched = true;
	}
	if (luna_get_double(parsed, "rightInset", &d)) {
		p->right_inset = d;
		insets_touched = true;
	}
	if (luna_get_double(parsed, "bottomInset", &d)) {
		p->bottom_inset = d;
		insets_touched = true;
	}

	if (insets_touched)
		p->insets_set = true;

	/* Borderless and explicit insets are mutually exclusive (-602). */
	if (p->borderless && p->insets_set) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_INSETS_WITH_BORDERLESS);
		return true;
	}

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool jobs_get_final_params(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	struct print_job *job;
	struct printable_area area;
	struct cups_caps *caps;
	int job_id, err = 0, dpi = 300;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	/* Render at the printer's own resolution when it advertises one. */
	caps = cups_backend_get_caps(job->printer_id, &err);
	if (caps) {
		if (caps->max_resolution > 0)
			dpi = caps->max_resolution;
		cups_caps_free(caps);
	}

	if (!job_compute_area(job->params.media_size, &job->params, dpi, &area,
	                      &err)) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	reply = jobject_create();
	put_ok(reply);
	put_params(reply, &job->params);

	jobject_put(reply, J_CSTR_TO_JVAL("paramsAdjusted"),
	            jboolean_create(area.params_adjusted));
	jobject_put(reply, J_CSTR_TO_JVAL("pageTopMargin"),
	            jnumber_create_f64(area.page_top_margin));
	jobject_put(reply, J_CSTR_TO_JVAL("pageLeftMargin"),
	            jnumber_create_f64(area.page_left_margin));
	jobject_put(reply, J_CSTR_TO_JVAL("pageRightMargin"),
	            jnumber_create_f64(area.page_right_margin));
	jobject_put(reply, J_CSTR_TO_JVAL("pageBottomMargin"),
	            jnumber_create_f64(area.page_bottom_margin));
	jobject_put(reply, J_CSTR_TO_JVAL("pixelUnits"),
	            jnumber_create_i32(area.pixel_units));
	jobject_put(reply, J_CSTR_TO_JVAL("rawWidth"),
	            jnumber_create_i32(area.raw_width));
	jobject_put(reply, J_CSTR_TO_JVAL("rawHeight"),
	            jnumber_create_i32(area.raw_height));
	jobject_put(reply, J_CSTR_TO_JVAL("width"),
	            jnumber_create_i32(area.width));
	jobject_put(reply, J_CSTR_TO_JVAL("height"),
	            jnumber_create_i32(area.height));
	jobject_put(reply, J_CSTR_TO_JVAL("renderInReverseOrder"),
	            jboolean_create(false));

	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static bool jobs_get_area_for_insets(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	struct print_job *job;
	struct print_params probe;
	struct printable_area area;
	int job_id, err = 0;
	double d;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	/*
	 * A what-if query: it must not mutate the job. Copy the current params
	 * by value and overlay only the insets that were supplied.
	 */
	probe = job->params;
	if (luna_get_double(parsed, "topInset", &d))
		probe.top_inset = d;
	if (luna_get_double(parsed, "leftInset", &d))
		probe.left_inset = d;
	if (luna_get_double(parsed, "rightInset", &d))
		probe.right_inset = d;
	if (luna_get_double(parsed, "bottomInset", &d))
		probe.bottom_inset = d;

	if (!job_compute_area(probe.media_size, &probe, 300, &area, &err)) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("width"), jnumber_create_i32(area.width));
	jobject_put(reply, J_CSTR_TO_JVAL("height"),
	            jnumber_create_i32(area.height));
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static bool jobs_new_temp_file(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	struct print_job *job;
	const char *extension;
	char *path;
	int job_id, err = 0;
	int size = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id) ||
	    !luna_get_string(parsed, "fileExtension", &extension)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	luna_get_int(parsed, "size", &size);

	path = job_manager_new_temp_file(s->jobs, job, extension, size, &err);
	if (!path) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("pathName"), jstring_create(path));
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	g_free(path);
	j_release(&parsed);

	return true;
}

static bool jobs_resize_temp_file(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	const char *path;
	GList *jobs, *l;
	int new_size, err = PM_ERR_FILE_NOT_IN_ANY_JOB;
	bool done = false;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "pathName", &path) ||
	    !luna_get_int(parsed, "newSize", &new_size)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	/* resizeTempFile identifies the file by path alone, with no jobID, so
	 * the owning job has to be found by search. */
	jobs = job_manager_list(s->jobs);
	for (l = jobs; l && !done; l = l->next) {
		if (job_manager_resize_temp_file(l->data, path, new_size, &err))
			done = true;
	}
	g_list_free(jobs);

	if (!done) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool jobs_add_file(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	struct print_job *job;
	const char *path;
	struct stat st;
	int job_id, current_page = 0, total_pages = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id) ||
	    !luna_get_string(parsed, "pathName", &path)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	if (job->phase != JOB_PHASE_OPEN) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_JOB_ALREADY_CLOSED);
		return true;
	}

	if (stat(path, &st) != 0) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_FILE_DOES_NOT_EXIST);
		return true;
	}

	if (st.st_size == 0) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_FILE_ZERO_LENGTH);
		return true;
	}

	/*
	 * Sniff the content rather than trusting the extension. A client that
	 * asked for a ".jpg" temp file and then wrote a PDF into it - or wrote
	 * nothing recognisable at all - is caught here rather than by the
	 * printer.
	 */
	if (!cups_backend_detect_format(path)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNSUPPORTED_MIME_TYPE);
		return true;
	}

	if (luna_get_int(parsed, "currentPage", &current_page)) {
		/*
		 * Pages must arrive in order; the original rejected a gap or a
		 * repeat with -615 rather than silently reordering.
		 */
		if (current_page != (int) g_list_length(job->files) + 1) {
			j_release(&parsed);
			reply_err(h, m, PM_ERR_OUT_OF_ORDER_CURRENTPAGE);
			return true;
		}
		job->current_page = current_page;
	}

	if (luna_get_int(parsed, "totalPages", &total_pages))
		job->total_pages = total_pages;

	job->files = g_list_append(job->files, g_strdup(path));

	post_page_info(s, job, "PAGESTARTINFO", (int) g_list_length(job->files));

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

/* Builds the IPP-side parameter block from the webOS-side one. */
static void build_cups_params(const struct print_job *job,
                              struct cups_job_params *out, int max_resolution)
{
	const struct print_params *p = &job->params;

	memset(out, 0, sizeof(*out));

	out->media = pm_media_webos_to_pwg(p->media_size);
	out->media_type = pm_mediatype_webos_to_ipp(p->media_type);
	out->media_source = pm_tray_webos_to_ipp(p->tray);
	out->sides = pm_duplex_webos_to_ipp(p->duplex);
	out->color_mode = pm_color_webos_to_ipp(p->color);
	out->quality = pm_quality_webos_to_ipp(p->print_quality);
	out->copies = p->num_copies;
	out->borderless = p->borderless;

	/* MaxDpi is the one quality keyword that also pins the resolution. */
	if (p->print_quality && !strcmp(p->print_quality, "MaxDpi"))
		out->resolution = max_resolution;

	/*
	 * autoScale/autoRotate collapse onto print-scaling. "auto" lets the
	 * printer rotate and fit; "none" prints at native size. IPP has no
	 * separate rotate control, which is fine - the only caller that sets
	 * autoRotate (ImagePrintJob) always sets autoScale too.
	 */
	if (p->auto_scale)
		out->print_scaling = "auto";
	else
		out->print_scaling = "none";

	if (p->insets_set && !p->borderless) {
		/* IPP margins are hundredths of a millimetre. */
		out->top_margin = (int) (p->top_inset * 2540.0);
		out->left_margin = (int) (p->left_inset * 2540.0);
		out->right_margin = (int) (p->right_inset * 2540.0);
		out->bottom_margin = (int) (p->bottom_inset * 2540.0);
		out->margins_valid = true;
	}
}

static bool submit_job(struct print_service *s, struct print_job *job, int *err)
{
	struct cups_job_params params;
	struct cups_caps *caps;
	int max_res = 0;
	int cups_id;

	if (!job->files) {
		*err = PM_ERR_NO_FILES_TO_PRINT;
		return false;
	}

	caps = cups_backend_get_caps(job->printer_id, err);
	if (caps) {
		max_res = caps->max_resolution;
		cups_caps_free(caps);
	}

	build_cups_params(job, &params, max_res);

	cups_id = cups_backend_submit(job->printer_id, job->description,
	                              job->files, &params, err);
	if (cups_id < 0)
		return false;

	job->cups_job_id = cups_id;
	job->phase = JOB_PHASE_SUBMITTED;

	if (job->total_pages < 0)
		job->total_pages = (int) g_list_length(job->files);

	post_job_state(s, job, "QUEUED", NULL, NULL);
	g_free(job->last_printer_state);
	job->last_printer_state = g_strdup("QUEUED");

	ensure_polling(s);

	return true;
}

static bool jobs_close(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	struct print_job *job;
	int job_id, err = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	if (job->phase != JOB_PHASE_OPEN) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_JOB_ALREADY_CLOSED);
		return true;
	}

	/*
	 * A close with no pages is not an error - a client that opened a job
	 * and then had nothing to print (or cancelled during rendering) closes
	 * it to tidy up. Report DONE so any subscriber stops waiting.
	 */
	if (!job->files) {
		job->phase = JOB_PHASE_DONE;
		post_job_state(s, job, "DONE", "Success", NULL);
		luna_service_message_reply_success(h, m);
		j_release(&parsed);
		return true;
	}

	if (!submit_job(s, job, &err)) {
		job->phase = JOB_PHASE_DONE;
		post_job_state(s, job, "DONE", "Error", NULL);
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool jobs_cancel(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	struct print_job *job;
	int job_id;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	if (job->phase == JOB_PHASE_SUBMITTED && job->cups_job_id > 0) {
		if (!cups_backend_cancel(job->printer_id, job->cups_job_id)) {
			j_release(&parsed);
			reply_err(h, m, PM_ERR_JOB_CANCEL_FAILED);
			return true;
		}
	}

	job->phase = JOB_PHASE_CANCELLED;
	post_job_state(s, job, "DONE", "Cancelled", NULL);

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool jobs_get_status(LSHandle *h, LSMessage *m, void *ctx)
{
	jvalue_ref parsed, reply;
	bool subscribed;

	(void) ctx;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	subscribed = luna_service_check_for_subscription_and_process(h, m);

	/*
	 * A plain non-subscribing call gets only the ack - no state. That
	 * looks odd but it is exactly what the device does, and the client
	 * library depends on it: PrintJob always subscribes and reads state
	 * from the subscription, never from this reply.
	 */
	reply = jobject_create();
	put_ok(reply);
	put_subscribed(reply, subscribed);
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static bool jobs_get_render_status(LSHandle *h, LSMessage *m, void *ctx)
{
	jvalue_ref parsed, reply;
	bool subscribed;

	(void) ctx;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	subscribed = luna_service_check_for_subscription_and_process(h, m);

	reply = jobject_create();
	put_ok(reply);
	put_subscribed(reply, subscribed);
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}

static bool jobs_set_render_status(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, event;
	struct print_job *job;
	const char *text = "";
	int job_id, code;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobID", &job_id) ||
	    !luna_get_int(parsed, "renderResultCode", &code)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	job = job_manager_find(s->jobs, job_id);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_UNKNOWN_JOB_ID);
		return true;
	}

	luna_get_string(parsed, "renderResultText", &text);

	job->render_result_code = code;
	job->render_result_set = true;
	g_free(job->render_result_text);
	job->render_result_text = g_strdup(text);

	/* Relay to render-status subscribers; this is how a rendering client
	 * learns its own completion has been recorded. */
	event = jobject_create();
	jobject_put(event, J_CSTR_TO_JVAL("jobID"), jnumber_create_i32(job_id));
	jobject_put(event, J_CSTR_TO_JVAL("renderResultCode"),
	            jnumber_create_i32(code));
	jobject_put(event, J_CSTR_TO_JVAL("renderResultText"),
	            jstring_create(text));
	luna_service_post_subscription(s->handle, CAT_JOBS,
	                               METHOD_GET_RENDER_STATUS, event);
	j_release(&event);

	luna_service_message_reply_success(h, m);
	j_release(&parsed);

	return true;
}

static bool jobs_list_all(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref reply, arr;
	GList *jobs, *l;

	arr = jarray_create(NULL);

	jobs = job_manager_list(s->jobs);
	for (l = jobs; l; l = l->next) {
		struct print_job *job = l->data;
		jvalue_ref o = jobject_create();

		jobject_put(o, J_CSTR_TO_JVAL("jobID"),
		            jnumber_create_i32(job->job_id));
		jobject_put(o, J_CSTR_TO_JVAL("printerID"),
		            jstring_create(job->printer_id ? job->printer_id : ""));
		jobject_put(o, J_CSTR_TO_JVAL("description"),
		            jstring_create(job->description ? job->description : ""));
		jobject_put(o, J_CSTR_TO_JVAL("appName"),
		            jstring_create(job->app_name ? job->app_name : ""));

		jarray_append(arr, o);
	}
	g_list_free(jobs);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("jobs"), arr);
	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	return true;
}

/*
 * allAtOnce: open, add every file, close, in one call.
 *
 * On the original this returned returnValue:true and then silently dropped the
 * job - captured on a TouchPad, jobID 2 vanished with no page produced and no
 * error ever surfaced. Implementing it as a wrapper over exactly the same job
 * machinery as the long-hand path means that failure mode cannot exist here:
 * anything that goes wrong comes back as a real error code, and the job shows
 * up in the same status stream as any other.
 */
static bool jobs_all_at_once(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply, paths;
	struct print_job *job;
	const char *printer_id, *description, *app_name;
	bool auto_scale = false;
	ssize_t i, n;
	int err = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerID", &printer_id) ||
	    !luna_get_string(parsed, "description", &description) ||
	    !luna_get_string(parsed, "appName", &app_name)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!jobject_get_exists(parsed, J_CSTR_TO_BUF("pathNames"), &paths) ||
	    !jis_array(paths)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	n = jarray_size(paths);
	if (n < 1) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_NO_FILES_TO_PRINT);
		return true;
	}

	job = job_manager_open(s->jobs, printer_id, description, app_name);
	if (!job) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_JOB_NO_JOB_HANDLES);
		return true;
	}

	if (luna_get_bool(parsed, "autoScale", &auto_scale))
		job->params.auto_scale = auto_scale;

	for (i = 0; i < n; i++) {
		jvalue_ref item = jarray_get(paths, i);
		raw_buffer buf;
		struct stat st;

		if (!jis_string(item)) {
			job_manager_remove(s->jobs, job->job_id);
			j_release(&parsed);
			reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
			return true;
		}

		buf = jstring_get_fast(item);

		if (stat(buf.m_str, &st) != 0) {
			job_manager_remove(s->jobs, job->job_id);
			j_release(&parsed);
			reply_err(h, m, PM_ERR_FILE_DOES_NOT_EXIST);
			return true;
		}

		if (st.st_size == 0) {
			job_manager_remove(s->jobs, job->job_id);
			j_release(&parsed);
			reply_err(h, m, PM_ERR_FILE_ZERO_LENGTH);
			return true;
		}

		if (!cups_backend_detect_format(buf.m_str)) {
			job_manager_remove(s->jobs, job->job_id);
			j_release(&parsed);
			reply_err(h, m, PM_ERR_UNSUPPORTED_MIME_TYPE);
			return true;
		}

		job->files = g_list_append(job->files, g_strdup(buf.m_str));
	}

	job->total_pages = (int) n;

	if (!submit_job(s, job, &err)) {
		post_job_state(s, job, "DONE", "Error", NULL);
		job_manager_remove(s->jobs, job->job_id);
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("jobID"),
	            jnumber_create_i32(job->job_id));
	luna_service_message_validate_and_send(h, m, reply);

	j_release(&reply);
	j_release(&parsed);

	return true;
}


/* ------------------------------------------------------------------ */
/* org.webosports.service.print - the Settings app's Print Manager     */
/* ------------------------------------------------------------------ */

/*
 * A second, flatter surface over the same machinery. Field names, the state
 * vocabulary and the method names all follow what
 * settings-common/qml/General/PrintManagerPage.qml documents and calls; the
 * page shows a "service unavailable" notice until these answer.
 */

static jvalue_ref ports_printers_payload(struct print_service *s)
{
	jvalue_ref reply, arr;
	const char *current;
	GList *l;

	arr = jarray_create(NULL);

	for (l = s->known_printers; l; l = l->next) {
		struct cups_printer *p = l->data;
		jvalue_ref o = jobject_create();

		jobject_put(o, J_CSTR_TO_JVAL("printerId"),
		            jstring_create(p->printer_id));
		jobject_put(o, J_CSTR_TO_JVAL("name"),
		            jstring_create(p->printer_name ? p->printer_name
		                                           : p->printer_id));
		jobject_put(o, J_CSTR_TO_JVAL("uri"),
		            jstring_create(p->uri ? p->uri : ""));
		jobject_put(o, J_CSTR_TO_JVAL("location"),
		            jstring_create(p->location ? p->location : ""));
		jobject_put(o, J_CSTR_TO_JVAL("makeAndModel"),
		            jstring_create(p->make_and_model ?
		                           p->make_and_model : ""));
		jobject_put(o, J_CSTR_TO_JVAL("state"),
		            jstring_create(p->state ? p->state : "idle"));
		/*
		 * The page splits the list on this: discovered printers are
		 * listed under "Auto Discovered" and cannot be swiped away,
		 * typed-in ones under "Manually Added" and can.
		 */
		jobject_put(o, J_CSTR_TO_JVAL("discovered"),
		            jboolean_create(p->is_discovered));

		jarray_append(arr, o);
	}

	current = printer_db_get_current(s->db);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("printers"), arr);
	jobject_put(reply, J_CSTR_TO_JVAL("defaultPrinterId"),
	            jstring_create(current ? current : ""));

	return reply;
}

static jvalue_ref ports_jobs_payload(void)
{
	jvalue_ref reply, arr;
	GList *jobs, *l;

	arr = jarray_create(NULL);

	jobs = cups_backend_list_jobs();
	for (l = jobs; l; l = l->next) {
		struct cups_queue_job *j = l->data;
		jvalue_ref o = jobject_create();

		jobject_put(o, J_CSTR_TO_JVAL("jobId"),
		            jnumber_create_i32(j->id));
		jobject_put(o, J_CSTR_TO_JVAL("title"),
		            jstring_create(j->title ? j->title : ""));
		jobject_put(o, J_CSTR_TO_JVAL("printerId"),
		            jstring_create(j->dest ? j->dest : ""));
		jobject_put(o, J_CSTR_TO_JVAL("printerName"),
		            jstring_create(j->dest ? j->dest : ""));
		jobject_put(o, J_CSTR_TO_JVAL("state"),
		            jstring_create(cups_backend_jobstate_string(j->ipp_state)));
		/*
		 * cups_job_t carries no impression counts, so the page falls
		 * back to a plain "Printing" rather than "3 of 8". Reporting
		 * zero is honest; inventing a page count would not be.
		 */
		jobject_put(o, J_CSTR_TO_JVAL("pages"), jnumber_create_i32(0));
		jobject_put(o, J_CSTR_TO_JVAL("completedPages"),
		            jnumber_create_i32(0));

		jarray_append(arr, o);
	}
	g_list_free_full(jobs, (GDestroyNotify) cups_queue_job_free);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("jobs"), arr);

	return reply;
}

static void ports_post_printers(struct print_service *s)
{
	jvalue_ref o;

	if (!s->ports_handle)
		return;

	o = ports_printers_payload(s);
	luna_service_post_subscription(s->ports_handle, CAT_ROOT,
	                               METHOD_LIST_PRINTERS, o);
	j_release(&o);
}

static void ports_post_jobs(struct print_service *s)
{
	jvalue_ref o;

	if (!s->ports_handle)
		return;

	o = ports_jobs_payload();
	luna_service_post_subscription(s->ports_handle, CAT_ROOT,
	                               METHOD_LIST_JOBS, o);
	j_release(&o);
}

static bool ports_list_printers(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref reply;
	bool subscribed;

	subscribed = luna_service_check_for_subscription_and_process(h, m);

	reply = ports_printers_payload(s);
	put_subscribed(reply, subscribed);
	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	return true;
}

static bool ports_list_jobs(LSHandle *h, LSMessage *m, void *ctx)
{
	jvalue_ref reply;
	bool subscribed;

	(void) ctx;

	subscribed = luna_service_check_for_subscription_and_process(h, m);

	reply = ports_jobs_payload();
	put_subscribed(reply, subscribed);
	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	return true;
}

static bool ports_add_printer(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed, reply;
	const char *name, *uri;
	char *queue_name;
	int err = 0;
	size_t i;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "name", &name) ||
	    !luna_get_string(parsed, "uri", &uri)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	/*
	 * A CUPS queue name cannot contain spaces, "/" or "#", but the page
	 * lets the user type anything as a display name. Fold the typed name
	 * into something CUPS accepts and keep the original as printer-info.
	 */
	queue_name = g_strdup(name);
	for (i = 0; queue_name[i]; i++) {
		if (!g_ascii_isalnum(queue_name[i]) && queue_name[i] != '_' &&
		    queue_name[i] != '-' && queue_name[i] != '.')
			queue_name[i] = '_';
	}

	if (!*queue_name) {
		g_free(queue_name);
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!cups_backend_add_queue(queue_name, name, uri, &err)) {
		g_free(queue_name);
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	/* Remember it on this network, the same list the webOS API reports. */
	printer_db_add(s->db, queue_name, name, uri, s->ssid, &err);

	reply = jobject_create();
	put_ok(reply);
	jobject_put(reply, J_CSTR_TO_JVAL("printerId"),
	            jstring_create(queue_name));
	luna_service_message_validate_and_send(h, m, reply);
	j_release(&reply);

	/* Re-browse now so the page updates without waiting for the timer. */
	browse_printers(s);
	ports_post_printers(s);

	g_free(queue_name);
	j_release(&parsed);

	return true;
}

static bool ports_remove_printer(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	const char *printer_id;
	int err = 0;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerId", &printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!cups_backend_remove_queue(printer_id, &err)) {
		j_release(&parsed);
		reply_err(h, m, err);
		return true;
	}

	printer_db_delete(s->db, printer_id, NULL);

	luna_service_message_reply_success(h, m);

	browse_printers(s);
	ports_post_printers(s);

	j_release(&parsed);

	return true;
}

static bool ports_set_default_printer(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	const char *printer_id;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_string(parsed, "printerId", &printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	/* Same stored value the webOS printers/setCurrent writes, so the two
	 * APIs cannot disagree about which printer is the default. */
	if (!printer_db_set_current(s->db, printer_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_SET_CURRENT_PRINTER_FAILED);
		return true;
	}

	luna_service_message_reply_success(h, m);
	ports_post_printers(s);

	j_release(&parsed);

	return true;
}

static bool ports_cancel_job(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	jvalue_ref parsed;
	GList *jobs, *l;
	int job_id;
	bool done = false;

	parsed = luna_service_message_parse_and_validate(LSMessageGetPayload(m));
	if (!parsed) {
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	if (!luna_get_int(parsed, "jobId", &job_id)) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_BAD_PARAM_SYNTAX);
		return true;
	}

	/* jobId here is the CUPS job id, so the destination has to be looked
	 * up before it can be cancelled. */
	jobs = cups_backend_list_jobs();
	for (l = jobs; l; l = l->next) {
		struct cups_queue_job *j = l->data;

		if (j->id == job_id) {
			done = cups_backend_cancel(j->dest, j->id);
			break;
		}
	}
	g_list_free_full(jobs, (GDestroyNotify) cups_queue_job_free);

	if (!done) {
		j_release(&parsed);
		reply_err(h, m, PM_ERR_JOB_CANCEL_FAILED);
		return true;
	}

	luna_service_message_reply_success(h, m);
	ports_post_jobs(s);

	j_release(&parsed);

	return true;
}

static bool ports_cancel_all_jobs(LSHandle *h, LSMessage *m, void *ctx)
{
	struct print_service *s = ctx;
	GList *jobs, *l;

	jobs = cups_backend_list_jobs();
	for (l = jobs; l; l = l->next) {
		struct cups_queue_job *j = l->data;

		cups_backend_cancel(j->dest, j->id);
	}
	g_list_free_full(jobs, (GDestroyNotify) cups_queue_job_free);

	luna_service_message_reply_success(h, m);
	ports_post_jobs(s);

	return true;
}

static LSMethod ports_methods[] = {
	{ METHOD_LIST_PRINTERS, ports_list_printers },
	{ METHOD_LIST_JOBS, ports_list_jobs },
	{ "addPrinter", ports_add_printer },
	{ "removePrinter", ports_remove_printer },
	{ "setDefaultPrinter", ports_set_default_printer },
	{ "cancelJob", ports_cancel_job },
	{ "cancelAllJobs", ports_cancel_all_jobs },
	{ NULL, NULL },
};

/* ------------------------------------------------------------------ */
/* registration                                                       */
/* ------------------------------------------------------------------ */

static LSMethod printer_methods[] = {
	{ "list", printers_list },
	{ "getCapabilities", printers_get_capabilities },
	{ "getCurrent", printers_get_current },
	{ "setCurrent", printers_set_current },
	{ "add", printers_add },
	{ "delete", printers_delete },
	{ "listAddedPrinters", printers_list_added },
	{ "listActive", printers_list_active },
	{ NULL, NULL },
};

static LSMethod job_methods[] = {
	{ "getCurrentPrintParams", jobs_get_current_params },
	{ "editPrintParams", jobs_edit_params },
	{ "getFinalParamsAndArea", jobs_get_final_params },
	{ "newTempFile", jobs_new_temp_file },
	{ "resizeTempFile", jobs_resize_temp_file },
	{ "open", jobs_open },
	{ "addFile", jobs_add_file },
	{ "close", jobs_close },
	{ "cancel", jobs_cancel },
	{ "getStatus", jobs_get_status },
	{ "getRenderStatus", jobs_get_render_status },
	{ "setRenderStatus", jobs_set_render_status },
	{ "allAtOnce", jobs_all_at_once },
	{ "getAreaForInsets", jobs_get_area_for_insets },
	{ "listAll", jobs_list_all },
	{ NULL, NULL },
};

struct print_service *print_service_create(void)
{
	struct print_service *service;
	LSError error;

	service = g_new0(struct print_service, 1);

	LSErrorInit(&error);

	/*
	 * Register under the legacy name. The replicated UI and every existing
	 * client address com.palm.printmgr, and claiming it costs nothing;
	 * com.webos.service.print is declared as an alias in the role file.
	 */
	if (!LSRegister(PRINT_SERVICE_LEGACY_NAME, &service->handle, &error)) {
		g_critical("Failed to register %s: %s",
		           PRINT_SERVICE_LEGACY_NAME, error.message);
		goto error;
	}

	if (!LSGmainAttach(service->handle, event_loop, &error)) {
		g_critical("Failed to attach to the main loop: %s",
		           error.message);
		goto error;
	}

	if (!LSRegisterCategory(service->handle, "/printers", printer_methods,
	                        NULL, NULL, &error)) {
		g_critical("Failed to register /printers: %s", error.message);
		goto error;
	}

	if (!LSCategorySetData(service->handle, "/printers", service, &error)) {
		g_critical("Could not set /printers data: %s", error.message);
		goto error;
	}

	if (!LSRegisterCategory(service->handle, "/jobs", job_methods, NULL,
	                        NULL, &error)) {
		g_critical("Failed to register /jobs: %s", error.message);
		goto error;
	}

	if (!LSCategorySetData(service->handle, "/jobs", service, &error)) {
		g_critical("Could not set /jobs data: %s", error.message);
		goto error;
	}

	/*
	 * Second bus name for the Settings page. A separate LSHandle is
	 * required - LSRegister binds one name per handle - but everything
	 * behind it is shared.
	 */
	if (!LSRegister(PORTS_SERVICE_NAME, &service->ports_handle, &error)) {
		g_warning("Failed to register %s: %s - the Settings Print "
		          "Manager page will report no service",
		          PORTS_SERVICE_NAME, error.message);
		LSErrorFree(&error);
		LSErrorInit(&error);
		service->ports_handle = NULL;
	} else if (!LSGmainAttach(service->ports_handle, event_loop, &error)) {
		g_warning("Failed to attach %s to the main loop: %s",
		          PORTS_SERVICE_NAME, error.message);
		goto error;
	} else if (!LSRegisterCategory(service->ports_handle, CAT_ROOT,
	                               ports_methods, NULL, NULL, &error)) {
		g_critical("Failed to register the %s category: %s",
		           PORTS_SERVICE_NAME, error.message);
		goto error;
	} else if (!LSCategorySetData(service->ports_handle, CAT_ROOT, service,
	                              &error)) {
		g_critical("Could not set %s data: %s", PORTS_SERVICE_NAME,
		           error.message);
		goto error;
	}

	service->db = printer_db_open(PRINTER_DB_PATH);
	service->jobs = job_manager_new(SPOOL_ROOT);

	/*
	 * Prime the printer list once at startup so the first subscriber gets
	 * an immediate answer, then re-browse periodically. CUPS does the mDNS
	 * work; this only diffs the result.
	 */
	service->known_printers = cups_backend_enumerate();
	service->browse_source = g_timeout_add_seconds(5, browse_printers,
	                                               service);

	return service;

error:
	LSErrorFree(&error);
	g_free(service);

	return NULL;
}

void print_service_free(struct print_service *service)
{
	LSError error;

	if (!service)
		return;

	LSErrorInit(&error);

	if (service->browse_source)
		g_source_remove(service->browse_source);
	if (service->poll_source)
		g_source_remove(service->poll_source);

	g_list_free_full(service->known_printers,
	                 (GDestroyNotify) cups_printer_free);

	job_manager_free(service->jobs);
	printer_db_close(service->db);

	if (service->handle && !LSUnregister(service->handle, &error)) {
		g_warning("Could not unregister service: %s", error.message);
		LSErrorFree(&error);
		LSErrorInit(&error);
	}

	if (service->ports_handle &&
	    !LSUnregister(service->ports_handle, &error)) {
		g_warning("Could not unregister %s: %s", PORTS_SERVICE_NAME,
		          error.message);
		LSErrorFree(&error);
	}

	g_free(service->ssid);
	g_free(service);
}

// vim:ts=4:sw=4:noexpandtab
