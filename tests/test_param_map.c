/* Unit tests for the webOS <-> IPP mapping layer.
 * Builds and runs on the host: see tests/run-tests.sh */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "param_map.h"
#include "print_errors.h"

static int failures;
static int checks;

static void ck_str(const char *what, const char *got, const char *want)
{
	checks++;
	if (want == NULL && got == NULL)
		return;
	if (want && got && !strcmp(got, want))
		return;

	failures++;
	printf("FAIL %-46s got=%-28s want=%s\n", what,
	       got ? got : "(null)", want ? want : "(null)");
}

static void ck_int(const char *what, int got, int want)
{
	checks++;
	if (got == want)
		return;

	failures++;
	printf("FAIL %-46s got=%-28d want=%d\n", what, got, want);
}

static void ck_bool(const char *what, bool got, bool want)
{
	checks++;
	if (got == want)
		return;

	failures++;
	printf("FAIL %-46s got=%-28s want=%s\n", what,
	       got ? "true" : "false", want ? "true" : "false");
}

int main(void)
{
	const char *ps, *js;

	/* --- media, forward --- */
	ck_str("media US_Letter", pm_media_webos_to_pwg("US_Letter"),
	       "na_letter_8.5x11in");
	ck_str("media ISO_A4", pm_media_webos_to_pwg("ISO_A4"),
	       "iso_a4_210x297mm");
	ck_str("media HAGAKI", pm_media_webos_to_pwg("HAGAKI"),
	       "jpn_hagaki_100x148mm");
	ck_str("media B_Tabloid", pm_media_webos_to_pwg("B_Tabloid"),
	       "na_ledger_11x17in");
	/* no PWG equivalent -> NULL, must not be guessed */
	ck_str("media Printable CD 5 inch",
	       pm_media_webos_to_pwg("Printable CD 5 inch"), NULL);
	ck_str("media Photo_4X12", pm_media_webos_to_pwg("Photo_4X12"), NULL);
	ck_str("media bogus", pm_media_webos_to_pwg("NoSuchSize"), NULL);
	ck_str("media NULL input", pm_media_webos_to_pwg(NULL), NULL);

	/* --- media, reverse --- */
	ck_str("media rev letter", pm_media_pwg_to_webos("na_letter_8.5x11in"),
	       "US_Letter");
	ck_str("media rev a4", pm_media_pwg_to_webos("iso_a4_210x297mm"),
	       "ISO_A4");
	/* 5x7 is aliased; reverse must yield the plain photo-tray form */
	ck_str("media rev 5x7 alias", pm_media_pwg_to_webos("na_5x7_5x7in"),
	       "Photo_5x7");
	ck_str("media rev unknown", pm_media_pwg_to_webos("iso_b2_500x707mm"),
	       NULL);

	/* round trip for every size that has a PWG name */
	{
		static const char *sizes[] = {
			"US_Letter", "US_Legal", "B_Tabloid", "Super B",
			"ISO_A3", "ISO_A4", "ISO_A6", "HAGAKI",
			"Photo_L", "Photo_4x6", "Photo_8x10", "Photo_10x15",
			NULL,
		};
		int i;

		for (i = 0; sizes[i]; i++) {
			const char *pwg = pm_media_webos_to_pwg(sizes[i]);
			char label[96];

			snprintf(label, sizeof(label), "media roundtrip %s",
			         sizes[i]);
			ck_str(label, pm_media_pwg_to_webos(pwg), sizes[i]);
		}
	}

	/* --- quality --- */
	ck_int("quality Fast", pm_quality_webos_to_ipp("Fast"), 3);
	ck_int("quality Normal", pm_quality_webos_to_ipp("Normal"), 4);
	ck_int("quality Best", pm_quality_webos_to_ipp("Best"), 5);
	ck_int("quality MaxDpi folds to high",
	       pm_quality_webos_to_ipp("MaxDpi"), 5);
	ck_int("quality Auto omits", pm_quality_webos_to_ipp("Auto"), 0);
	ck_int("quality NULL", pm_quality_webos_to_ipp(NULL), 0);
	ck_str("quality rev 3", pm_quality_ipp_to_webos(3), "Fast");
	ck_str("quality rev 4", pm_quality_ipp_to_webos(4), "Normal");
	ck_str("quality rev 5", pm_quality_ipp_to_webos(5), "Best");
	ck_str("quality rev junk", pm_quality_ipp_to_webos(99), "Auto");

	/* --- duplex --- */
	ck_str("duplex None", pm_duplex_webos_to_ipp("None"), "one-sided");
	ck_str("duplex Book", pm_duplex_webos_to_ipp("Book"),
	       "two-sided-long-edge");
	ck_str("duplex Tablet", pm_duplex_webos_to_ipp("Tablet"),
	       "two-sided-short-edge");
	ck_str("duplex rev long", pm_duplex_ipp_to_webos("two-sided-long-edge"),
	       "Book");

	/* --- color --- */
	ck_str("color Mono", pm_color_webos_to_ipp("Mono"), "monochrome");
	ck_str("color Color", pm_color_webos_to_ipp("Color"), "color");
	ck_str("color rev", pm_color_ipp_to_webos("monochrome"), "Mono");

	/* --- media type --- */
	ck_str("mediatype Plain", pm_mediatype_webos_to_ipp("Plain"),
	       "stationery");
	ck_str("mediatype Photo Glossy",
	       pm_mediatype_webos_to_ipp("Photo Glossy"), "photographic-glossy");
	ck_str("mediatype PrintableCD has no IPP name",
	       pm_mediatype_webos_to_ipp("PrintableCD"), NULL);

	/* --- tray --- */
	ck_str("tray Tray1", pm_tray_webos_to_ipp("Tray1"), "tray-1");
	ck_str("tray Photo", pm_tray_webos_to_ipp("Photo"), "photo");
	ck_str("tray Unspecified omits attribute",
	       pm_tray_webos_to_ipp("Unspecified"), NULL);
	ck_bool("tray Unspecified still valid input",
	        pm_tray_is_valid("Unspecified"), true);

	/* --- validation accepts every keyword the real daemon accepted --- */
	ck_bool("valid media US_Letter", pm_media_is_valid("US_Letter"), true);
	ck_bool("valid media CD (accepted, unmapped)",
	        pm_media_is_valid("Printable CD 5 inch"), true);
	ck_bool("invalid media", pm_media_is_valid("NoSuchSize"), false);
	ck_bool("valid drytime Minimum", pm_drytime_is_valid("Minimum"), true);
	ck_bool("invalid drytime", pm_drytime_is_valid("Instant"), false);
	ck_bool("valid quality MaxDpi", pm_quality_is_valid("MaxDpi"), true);
	ck_bool("invalid quality", pm_quality_is_valid("Ultra"), false);
	ck_bool("valid color Mono", pm_color_is_valid("Mono"), true);
	ck_bool("invalid color", pm_color_is_valid("Sepia"), false);
	ck_bool("valid duplex Tablet", pm_duplex_is_valid("Tablet"), true);
	ck_bool("NULL is never valid", pm_media_is_valid(NULL), false);

	/* --- job state --- */
	pm_jobstate_ipp_to_webos(3, &ps, &js);
	ck_str("jobstate 3 pending", ps, "QUEUED");
	ck_str("jobstate 3 no jobStatus", js, NULL);

	pm_jobstate_ipp_to_webos(5, &ps, &js);
	ck_str("jobstate 5 processing", ps, "RUNNING");
	ck_str("jobstate 5 no jobStatus", js, NULL);

	pm_jobstate_ipp_to_webos(6, &ps, &js);
	ck_str("jobstate 6 stopped", ps, "BLOCKED");

	pm_jobstate_ipp_to_webos(7, &ps, &js);
	ck_str("jobstate 7 canceled", ps, "DONE");
	ck_str("jobstate 7 status", js, "Cancelled");

	pm_jobstate_ipp_to_webos(8, &ps, &js);
	ck_str("jobstate 8 aborted", ps, "DONE");
	ck_str("jobstate 8 status", js, "Error");

	pm_jobstate_ipp_to_webos(9, &ps, &js);
	ck_str("jobstate 9 completed", ps, "DONE");
	ck_str("jobstate 9 status", js, "Success");

	/* --- state reasons --- */
	ck_str("reason media-empty", pm_reason_ipp_to_webos("media-empty", false),
	       "Out_Of_Paper");
	ck_str("reason media-empty-error severity suffix",
	       pm_reason_ipp_to_webos("media-empty-error", false), "Out_Of_Paper");
	ck_str("reason media-empty-warning",
	       pm_reason_ipp_to_webos("media-empty-warning", false),
	       "Out_Of_Paper");
	ck_str("reason media-jam", pm_reason_ipp_to_webos("media-jam", false),
	       "Jammed");
	ck_str("reason cover-open", pm_reason_ipp_to_webos("cover-open", false),
	       "Door_Open");
	ck_str("reason supply-empty ink",
	       pm_reason_ipp_to_webos("marker-supply-empty", false),
	       "Out_Of_Ink");
	ck_str("reason supply-empty toner",
	       pm_reason_ipp_to_webos("marker-supply-empty", true),
	       "Out_Of_Toner");
	ck_str("reason supply-low ink",
	       pm_reason_ipp_to_webos("marker-supply-low", false), "Low_On_Ink");
	ck_str("reason supply-low toner",
	       pm_reason_ipp_to_webos("marker-supply-low", true),
	       "Low_On_Toner");
	ck_str("reason offline", pm_reason_ipp_to_webos("offline", false),
	       "Unable_To_Connect");
	ck_str("reason none -> NULL", pm_reason_ipp_to_webos("none", false),
	       NULL);
	ck_str("reason unknown -> NULL",
	       pm_reason_ipp_to_webos("banana-peel-detected", false), NULL);
	ck_str("reason NULL input", pm_reason_ipp_to_webos(NULL, false), NULL);
	/* must not prefix-match a longer unrelated reason */
	ck_str("reason media-emptyish is not media-empty",
	       pm_reason_ipp_to_webos("media-emptyish", false), NULL);

	/* --- error texts match the shipped daemon byte for byte --- */
	ck_str("err -610", pm_error_text(-610),
	       "programming: incorrect parameter syntax");
	ck_str("err -617", pm_error_text(-617), "programming: unknown job ID");
	ck_str("err -202", pm_error_text(-202),
	       "user_system: unknown printer ID - may no longer be on access point");
	ck_str("err -206", pm_error_text(-206), "user: not a valid IP address");
	ck_str("err unknown code falls back", pm_error_text(-12345),
	       "unknown: unknown job error");

	printf("\n%d checks, %d failures\n", checks, failures);

	return failures ? 1 : 0;
}
