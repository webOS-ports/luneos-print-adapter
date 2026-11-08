/*
 * The page numbers and the queue name a job reports: job_reported_page,
 * job_reported_total_pages and job_title.
 */

#include <stdio.h>
#include <string.h>

#include "job_manager.h"

static int failures, checks;

static void ck(const char *what, int got, int want)
{
	checks++;
	if (got == want)
		return;
	failures++;
	printf("FAIL %-44s got=%-10d want=%d\n", what, got, want);
}

static void cks(const char *what, const char *got, const char *want)
{
	checks++;
	if ((!got && !want) || (got && want && !strcmp(got, want)))
		return;
	failures++;
	printf("FAIL %-44s got=%s want=%s\n", what, got ? got : "(null)",
	       want ? want : "(null)");
}

int main(void)
{
	struct print_job job;

	/* Pictures: a file is a page, so the app's numbers stand. */
	memset(&job, 0, sizeof(job));
	job.current_page = 2;
	job.total_pages = 5;
	ck("images current", job_reported_page(&job, job.current_page), 2);
	ck("images total", job_reported_total_pages(&job), 5);

	/* Not yet known (-1 until the job closes) is reported as 0. */
	job.total_pages = -1;
	ck("images total unknown", job_reported_total_pages(&job), 0);

	/* A PDF: one file of any number of pages. Atlas and the PDF app say
	 * 1 of 1 for it, which is the file, not the page. */
	memset(&job, 0, sizeof(job));
	job.has_pdf = true;
	job.current_page = 1;
	job.total_pages = 1;
	ck("pdf current", job_reported_page(&job, job.current_page), 0);
	ck("pdf total", job_reported_total_pages(&job), 0);

	/* The queue name: description, else the app, else the default. */
	memset(&job, 0, sizeof(job));
	job.description = "webOS - Wikipedia";
	job.app_name = "Browser";
	cks("title from description", job_title(&job), "webOS - Wikipedia");
	job.description = "";
	cks("empty description: the app", job_title(&job), "Browser");
	job.description = NULL;
	cks("no description: the app", job_title(&job), "Browser");
	job.app_name = "";
	cks("nothing: default", job_title(&job), NULL);

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
