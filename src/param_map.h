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

#ifndef PARAM_MAP_H_
#define PARAM_MAP_H_

#include <stdbool.h>

/*
 * Translation between the webOS printmgr keyword vocabulary and IPP/PWG.
 *
 * Deliberately free of any cups, luna-service2 or pbnjson dependency so it can
 * be unit tested on the build host - see tests/test_param_map.c. Everything
 * here is a pure function over strings.
 *
 * The webOS side of every table is authoritative: it comes from the
 * *_to_keyword_trans_table arrays in the shipped printmgrd binary, so it is the
 * exact set of strings the replicated UI and the enyo printdialog will send.
 */

/* Media size. NULL return = no sane PWG equivalent, drop from capabilities. */
const char *pm_media_webos_to_pwg(const char *webos);
const char *pm_media_pwg_to_webos(const char *pwg);

/* print-quality: IPP enum 3=draft 4=normal 5=high. 0 = omit the attribute. */
int pm_quality_webos_to_ipp(const char *webos);
const char *pm_quality_ipp_to_webos(int ipp);

/* sides */
const char *pm_duplex_webos_to_ipp(const char *webos);
const char *pm_duplex_ipp_to_webos(const char *ipp);

/* print-color-mode */
const char *pm_color_webos_to_ipp(const char *webos);
const char *pm_color_ipp_to_webos(const char *ipp);

/* media-type */
const char *pm_mediatype_webos_to_ipp(const char *webos);
const char *pm_mediatype_ipp_to_webos(const char *ipp);

/* media-source */
const char *pm_tray_webos_to_ipp(const char *webos);
const char *pm_tray_ipp_to_webos(const char *ipp);

/*
 * IPP job-state (RFC 8011 s5.3.7) to the webOS pair. printer_state is always
 * set; job_status is set only for terminal states and is NULL otherwise,
 * matching the daemon, which omits jobStatus until printerState is DONE.
 */
void pm_jobstate_ipp_to_webos(int job_state, const char **printer_state,
                              const char **job_status);

/*
 * A single printer-state-reason to a webOS blockedReasons entry, or NULL if it
 * has no equivalent. Supply-related reasons map to ink or toner depending on
 * is_toner, which the caller derives from marker-types.
 */
const char *pm_reason_ipp_to_webos(const char *reason, bool is_toner);

/* Validation, for rejecting editPrintParams with the right band of error. */
bool pm_media_is_valid(const char *webos);
bool pm_mediatype_is_valid(const char *webos);
bool pm_quality_is_valid(const char *webos);
bool pm_duplex_is_valid(const char *webos);
bool pm_color_is_valid(const char *webos);
bool pm_tray_is_valid(const char *webos);
bool pm_drytime_is_valid(const char *webos);

#endif

// vim:ts=4:sw=4:noexpandtab
