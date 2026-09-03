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
#include <stddef.h>

#include "param_map.h"

struct kv {
	const char *webos;
	const char *ipp;
};

static const char *lookup_fwd(const struct kv *t, const char *k)
{
	int i;

	if (!k)
		return NULL;

	for (i = 0; t[i].webos; i++) {
		if (!strcmp(t[i].webos, k))
			return t[i].ipp;
	}

	return NULL;
}

static const char *lookup_rev(const struct kv *t, const char *k)
{
	int i;

	if (!k)
		return NULL;

	for (i = 0; t[i].webos; i++) {
		if (t[i].ipp && !strcmp(t[i].ipp, k))
			return t[i].webos;
	}

	return NULL;
}

static bool in_table(const struct kv *t, const char *k)
{
	int i;

	if (!k)
		return false;

	for (i = 0; t[i].webos; i++) {
		if (!strcmp(t[i].webos, k))
			return true;
	}

	return false;
}

/*
 * Media sizes, webOS keyword to PWG 5101.1 self-describing name.
 *
 * A NULL ipp side means "we accept the keyword from a client but there is no
 * standard PWG name for it". Those are the HP-specific photo and CD-tray sizes;
 * they get filtered out of printers/getCapabilities rather than guessed at,
 * because inventing a media name a printer will reject is worse than not
 * offering the size.
 *
 * Photo_5x7_MainTray is the same sheet as Photo_5x7 fed from the main tray
 * rather than the photo tray - in IPP that distinction is media-source, not
 * media, so both map to the same PWG name and the tray is carried separately.
 */
static const struct kv media_table[] = {
	{ "US_Letter",		"na_letter_8.5x11in" },
	{ "US_Legal",		"na_legal_8.5x14in" },
	{ "B_Tabloid",		"na_ledger_11x17in" },
	{ "Super B",		"na_super-b_13x19in" },
	{ "ISO_A3",		"iso_a3_297x420mm" },
	{ "ISO_A4",		"iso_a4_210x297mm" },
	{ "ISO_A6",		"iso_a6_105x148mm" },
	{ "HAGAKI",		"jpn_hagaki_100x148mm" },
	{ "Photo_L",		"oe_photo-l_3.5x5in" },
	{ "Photo_4x6",		"na_index-4x6_4x6in" },
	{ "Photo_5x7",		"na_5x7_5x7in" },
	{ "Photo_5x7_MainTray",	"na_5x7_5x7in" },
	{ "Photo_8x10",		"na_govt-letter_8x10in" },
	{ "Photo_10x15",	"om_photo-10x15_100x150mm" },
	{ "Photo_11x14",	NULL },
	{ "Photo_4x8",		NULL },
	{ "Photo_4X12",		NULL },
	{ "Printable CD 3.5 inch", NULL },
	{ "Printable CD 5 inch", NULL },
	{ "None",		NULL },
	{ NULL, NULL },
};

const char *pm_media_webos_to_pwg(const char *webos)
{
	return lookup_fwd(media_table, webos);
}

const char *pm_media_pwg_to_webos(const char *pwg)
{
	/*
	 * Photo_5x7 and Photo_5x7_MainTray share a PWG name; the forward-order
	 * scan returns Photo_5x7, which is the right default when no tray was
	 * requested.
	 */
	return lookup_rev(media_table, pwg);
}

bool pm_media_is_valid(const char *webos)
{
	return in_table(media_table, webos);
}

/* media-type */
static const struct kv mediatype_table[] = {
	{ "Plain",		"stationery" },
	{ "Special",		"stationery-fine" },
	{ "Photo",		"photographic" },
	{ "Transparency",	"transparency" },
	{ "Iron-On",		"iron-on" },
	{ "Iron-On Mirror",	"iron-on-mirror" },
	{ "Advanced Photo",	"photographic-high-gloss" },
	{ "Fast Transparency",	"transparency" },
	{ "Brochure Glossy",	"stationery-coated" },
	{ "Brochure Matte",	"stationery-heavyweight" },
	{ "Photo Glossy",	"photographic-glossy" },
	{ "Photo Matte",	"photographic-matte" },
	{ "Premium Photo",	"photographic-high-gloss" },
	{ "Other Photo",	"photographic" },
	{ "PrintableCD",	NULL },
	{ "Premium Presentation", "stationery-heavyweight-coated" },
	{ "None",		NULL },
	{ NULL, NULL },
};

const char *pm_mediatype_webos_to_ipp(const char *webos)
{
	return lookup_fwd(mediatype_table, webos);
}

const char *pm_mediatype_ipp_to_webos(const char *ipp)
{
	return lookup_rev(mediatype_table, ipp);
}

bool pm_mediatype_is_valid(const char *webos)
{
	return in_table(mediatype_table, webos);
}

/*
 * print-quality. IPP has three enum values; webOS has five keywords, so MaxDpi
 * folds onto high (the caller additionally pins printer-resolution to the
 * maximum the printer advertises) and Auto means "say nothing and let the
 * printer decide".
 */
int pm_quality_webos_to_ipp(const char *webos)
{
	if (!webos)
		return 0;
	if (!strcmp(webos, "Fast"))
		return 3;
	if (!strcmp(webos, "Normal"))
		return 4;
	if (!strcmp(webos, "Best"))
		return 5;
	if (!strcmp(webos, "MaxDpi"))
		return 5;
	if (!strcmp(webos, "Auto"))
		return 0;

	return 0;
}

const char *pm_quality_ipp_to_webos(int ipp)
{
	switch (ipp) {
	case 3:
		return "Fast";
	case 4:
		return "Normal";
	case 5:
		return "Best";
	default:
		return "Auto";
	}
}

bool pm_quality_is_valid(const char *webos)
{
	static const struct kv t[] = {
		{ "Fast", NULL }, { "Normal", NULL }, { "Best", NULL },
		{ "MaxDpi", NULL }, { "Auto", NULL }, { NULL, NULL },
	};

	return in_table(t, webos);
}

/* sides */
static const struct kv duplex_table[] = {
	{ "None",	"one-sided" },
	{ "Book",	"two-sided-long-edge" },
	{ "Tablet",	"two-sided-short-edge" },
	{ NULL, NULL },
};

const char *pm_duplex_webos_to_ipp(const char *webos)
{
	return lookup_fwd(duplex_table, webos);
}

const char *pm_duplex_ipp_to_webos(const char *ipp)
{
	return lookup_rev(duplex_table, ipp);
}

bool pm_duplex_is_valid(const char *webos)
{
	return in_table(duplex_table, webos);
}

/* print-color-mode */
static const struct kv color_table[] = {
	{ "Mono",	"monochrome" },
	{ "Color",	"color" },
	{ NULL, NULL },
};

const char *pm_color_webos_to_ipp(const char *webos)
{
	return lookup_fwd(color_table, webos);
}

const char *pm_color_ipp_to_webos(const char *ipp)
{
	return lookup_rev(color_table, ipp);
}

bool pm_color_is_valid(const char *webos)
{
	return in_table(color_table, webos);
}

/*
 * media-source. "Unspecified" and "Auto" both mean "printer picks"; only Auto
 * has a real IPP name, so Unspecified maps to NULL and the attribute is simply
 * left off, which is the correct IPP way to say it.
 */
static const struct kv tray_table[] = {
	{ "Unspecified",	NULL },
	{ "Auto",		"auto" },
	{ "Tray1",		"tray-1" },
	/* "main" is what IPP Everywhere printers most often call the primary
	 * cassette; without this the tray list came back missing its main
	 * tray on a real queue. Aliased after tray-1 so the forward mapping
	 * still produces the canonical name. */
	{ "Tray1",		"main" },
	{ "Tray2",		"tray-2" },
	{ "Photo",		"photo" },
	{ "Manual",		"manual" },
	/* The bypass/multipurpose tray is hand-fed, so it presents as Manual. */
	{ "Manual",		"by-pass-tray" },
	{ "Manual Env",		"envelope-manual" },
	{ "Manual CD",		NULL },
	{ "Optional",		"tray-3" },
	{ "Continuous",		"continuous" },
	{ "Preload",		"pre-printed" },
	{ NULL, NULL },
};

const char *pm_tray_webos_to_ipp(const char *webos)
{
	return lookup_fwd(tray_table, webos);
}

const char *pm_tray_ipp_to_webos(const char *ipp)
{
	return lookup_rev(tray_table, ipp);
}

bool pm_tray_is_valid(const char *webos)
{
	return in_table(tray_table, webos);
}

/*
 * dryTime has no IPP counterpart at all - it was an HP inkjet notion. Accept
 * the keyword so a client that sends it is not rejected, then ignore it.
 */
bool pm_drytime_is_valid(const char *webos)
{
	static const struct kv t[] = {
		{ "Normal", NULL }, { "Lower", NULL }, { "Minimum", NULL },
		{ NULL, NULL },
	};

	return in_table(t, webos);
}

void pm_jobstate_ipp_to_webos(int job_state, const char **printer_state,
                              const char **job_status)
{
	*job_status = NULL;

	switch (job_state) {
	case 3:	/* pending */
	case 4:	/* pending-held */
		*printer_state = "QUEUED";
		break;
	case 5:	/* processing */
		*printer_state = "RUNNING";
		break;
	case 6:	/* processing-stopped */
		*printer_state = "BLOCKED";
		break;
	case 7:	/* canceled */
		*printer_state = "DONE";
		*job_status = "Cancelled";
		break;
	case 8:	/* aborted */
		*printer_state = "DONE";
		*job_status = "Error";
		break;
	case 9:	/* completed */
		*printer_state = "DONE";
		*job_status = "Success";
		break;
	default:
		*printer_state = "QUEUED";
		break;
	}
}

const char *pm_reason_ipp_to_webos(const char *reason, bool is_toner)
{
	size_t len;

	if (!reason)
		return NULL;

	/*
	 * printer-state-reasons carry an optional severity suffix
	 * (-report/-warning/-error). Compare against the stem so
	 * "media-empty-error" and "media-empty" behave the same.
	 */
	len = strlen(reason);

	#define STEM_IS(s) \
		(!strncmp(reason, s, strlen(s)) && \
		 (len == strlen(s) || reason[strlen(s)] == '-'))

	if (STEM_IS("media-empty") || STEM_IS("media-needed") ||
	    STEM_IS("input-media-supply-empty"))
		return "Out_Of_Paper";

	if (STEM_IS("media-jam") || STEM_IS("jam"))
		return "Jammed";

	if (STEM_IS("door-open") || STEM_IS("cover-open") ||
	    STEM_IS("interlock-open"))
		return "Door_Open";

	if (STEM_IS("marker-supply-empty") || STEM_IS("toner-empty"))
		return is_toner ? "Out_Of_Toner" : "Out_Of_Ink";

	if (STEM_IS("marker-supply-low") || STEM_IS("toner-low"))
		return is_toner ? "Low_On_Toner" : "Low_On_Ink";

	if (STEM_IS("service-request") || STEM_IS("moving-to-paused") ||
	    STEM_IS("paused"))
		return "Service_Request";

	if (STEM_IS("offline") || STEM_IS("shutdown") ||
	    STEM_IS("connecting-to-device") || STEM_IS("timed-out"))
		return "Unable_To_Connect";

	#undef STEM_IS

	return NULL;
}

// vim:ts=4:sw=4:noexpandtab
