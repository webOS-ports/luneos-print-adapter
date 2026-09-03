/* Verifies job_compute_area against values captured from a real TouchPad. */

#include <stdio.h>
#include <string.h>

#include "job_manager.h"
#include "print_errors.h"

static int failures, checks;

static void ck(const char *what, int got, int want)
{
	checks++;
	if (got == want)
		return;
	failures++;
	printf("FAIL %-44s got=%-10d want=%d\n", what, got, want);
}

int main(void)
{
	struct print_params p;
	struct printable_area a;
	int err = 0;

	/* --- captured: US_Letter, insets 0.5, 300dpi --- */
	print_params_init(&p);
	p.top_inset = p.left_inset = p.right_inset = p.bottom_inset = 0.5;
	p.insets_set = true;

	if (!job_compute_area("US_Letter", &p, 300, &a, &err)) {
		printf("FAIL letter/0.5 returned false err=%d\n", err);
		return 1;
	}
	ck("letter 0.5 pixelUnits", a.pixel_units, 300);
	ck("letter 0.5 rawWidth",  a.raw_width,  2475);
	ck("letter 0.5 rawHeight", a.raw_height, 3225);
	ck("letter 0.5 width",     a.width,      2251);
	ck("letter 0.5 height",    a.height,     3001);
	ck("letter 0.5 not adjusted", a.params_adjusted ? 1 : 0, 0);
	print_params_clear(&p);

	/* --- captured: getAreaForInsets US_Letter, insets 0.25 --- */
	print_params_init(&p);
	p.top_inset = p.left_inset = p.right_inset = p.bottom_inset = 0.25;
	p.insets_set = true;

	if (!job_compute_area("US_Letter", &p, 300, &a, &err)) {
		printf("FAIL letter/0.25 returned false err=%d\n", err);
		return 1;
	}
	ck("letter 0.25 width",  a.width,  2401);
	ck("letter 0.25 height", a.height, 3151);
	print_params_clear(&p);

	/* --- inset below the hardware margin clamps and flags --- */
	print_params_init(&p);
	p.top_inset = p.left_inset = p.right_inset = p.bottom_inset = 0.0;
	p.insets_set = true;

	if (!job_compute_area("US_Letter", &p, 300, &a, &err)) {
		printf("FAIL letter/0 returned false err=%d\n", err);
		return 1;
	}
	ck("letter 0 clamps to margin", a.params_adjusted ? 1 : 0, 1);
	ck("letter 0 width == raw+1", a.width, a.raw_width + 1);
	print_params_clear(&p);

	/* --- borderless uses the whole sheet --- */
	print_params_init(&p);
	p.borderless = true;

	if (!job_compute_area("US_Letter", &p, 300, &a, &err)) {
		printf("FAIL letter/borderless returned false err=%d\n", err);
		return 1;
	}
	ck("borderless width",  a.width,  2550);	/* 8.5 * 300 */
	ck("borderless height", a.height, 3300);	/* 11  * 300 */
	ck("borderless zero margin", (int) (a.page_top_margin * 1000), 0);
	print_params_clear(&p);

	/* --- A4 keeps its metric definition --- */
	print_params_init(&p);
	if (!job_compute_area("ISO_A4", &p, 300, &a, &err)) {
		printf("FAIL a4 returned false err=%d\n", err);
		return 1;
	}
	/* (210mm - 0.25in) * 300 = (8.2677 - 0.25) * 300 = 2405 */
	ck("a4 rawWidth", a.raw_width, 2405);
	/* (297mm - 0.25in) * 300 = (11.6929 - 0.25) * 300 = 3433 */
	ck("a4 rawHeight", a.raw_height, 3433);
	print_params_clear(&p);

	/* --- insets that consume the sheet are rejected --- */
	print_params_init(&p);
	p.top_inset = p.bottom_inset = 20.0;
	p.left_inset = p.right_inset = 0.5;
	p.insets_set = true;
	err = 0;
	if (job_compute_area("US_Letter", &p, 300, &a, &err)) {
		printf("FAIL oversized insets should have failed\n");
		failures++;
	} else {
		ck("oversized insets -> -613", err, PM_ERR_INSET_OUTSIDE_AREA);
	}
	print_params_clear(&p);

	/* --- unknown media --- */
	print_params_init(&p);
	err = 0;
	if (job_compute_area("NoSuchSize", &p, 300, &a, &err)) {
		printf("FAIL unknown media should have failed\n");
		failures++;
	} else {
		ck("unknown media -> -608", err, PM_ERR_BAD_MEDIA_SIZE);
	}
	print_params_clear(&p);

	/* --- defaults match what a fresh job reported on device --- */
	print_params_init(&p);
	checks++;
	if (strcmp(p.media_size, "US_Letter") || strcmp(p.media_type, "Plain") ||
	    strcmp(p.print_quality, "Normal") || strcmp(p.duplex, "None") ||
	    strcmp(p.dry_time, "Normal") || strcmp(p.color, "Color") ||
	    strcmp(p.tray, "Auto") || p.num_copies != 1) {
		printf("FAIL fresh job defaults do not match capture\n");
		failures++;
	}
	ck("default inset is the hardware margin",
	   (int) (p.top_inset * 1000), 125);
	print_params_clear(&p);

	printf("\n%d checks, %d failures\n", checks, failures);

	return failures ? 1 : 0;
}
