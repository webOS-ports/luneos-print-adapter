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

#include <stddef.h>

#include "print_errors.h"

/*
 * Error strings are reproduced verbatim from the original daemon, typos and
 * all ("no reponse", "system:unable" without the space). Anything parsing
 * errorText rather than errorCode is already broken, but matching byte for
 * byte costs nothing and removes one way for a port to differ.
 */
static const struct {
	int code;
	const char *text;
} error_texts[] = {
	{ PM_ERR_PRINTER_UNKNOWN_ID,		"user_system: unknown printer ID - may no longer be on access point" },
	{ PM_ERR_PRINTER_NO_RESPONSE_MANUAL,	"user: no reponse from manually added printer (IP may be incorrect or printer offline)" },
	{ PM_ERR_PRINTER_DUPLICATE_ID,		"user: printer ID already exists" },
	{ PM_ERR_PRINTER_DUPLICATE_IP,		"user: printer address already exists (manually added" },
	{ PM_ERR_PRINTER_IP_NOT_VALID,		"user: not a valid IP address" },
	{ PM_ERR_JOB_TEMP_FILE_NO_ROOM,		"recoverable: not enough room in file cache for temp file" },
	{ PM_ERR_PRINTER_NOT_SUPPORTED,		"user: printer is not supported (no pcl3gui or pcl5)" },
	{ PM_ERR_JOB_NO_JOB_HANDLES,		"recoverable: no job handles" },
	{ PM_ERR_PRINTER_DUPLICATE_IP_ZERO,	"user: printer address already exists (zeroconf added)" },
	{ PM_ERR_NO_WIFI_CONNECTION,		"user: no Wi-Fi connection" },
	{ PM_ERR_JOB_UNKNOWN,			"unknown: unknown job error" },

	{ PM_ERR_COMM_FAILED,			"communication: could not communicate with printer" },
	{ PM_ERR_COMM_GET_CAPS_FAILED,		"communication: failed to get printer capabilities" },
	{ PM_ERR_COMM_NO_RESPONSE,		"communication: no reponse from printer" },
	{ PM_ERR_COMM_ADDRESS_INVALID,		"communication: printer address no longer valid can't access" },
	{ PM_ERR_COMM_NO_PING_RESPONSE,		"communication: no response to ping before getting printer capabilities" },
	{ PM_ERR_COMM_ADDRESS_UNKNOWN,		"communication: printer address is unknown" },

	{ PM_ERR_JOB_ALREADY_CLOSED,		"info: action invalid, job already closed" },
	{ PM_ERR_JOB_CANCEL_REQUESTED,		"info: cancel requested for job" },
	{ PM_ERR_SET_CURRENT_PRINTER_FAILED,	"info: set currentPrinter failed" },

	{ PM_ERR_FILE_ZERO_LENGTH,		"programming: 0 length file" },
	{ PM_ERR_INSETS_WITH_BORDERLESS,	"programming: can't set insets with borderless" },
	{ PM_ERR_EMPTY_FILE_EXTENSION,		"programming: empty file extension" },
	{ PM_ERR_FILE_DOES_NOT_EXIST,		"programming: file does not exist" },
	{ PM_ERR_BAD_COLOR,			"programming: incorrect color" },
	{ PM_ERR_BAD_DRYTIME,			"programming: incorrect drytime" },
	{ PM_ERR_BAD_DUPLEX,			"programming: incorrect duplex" },
	{ PM_ERR_BAD_MEDIA_SIZE,		"programming: incorrect mediaSize" },
	{ PM_ERR_BAD_MEDIA_TYPE,		"programming: incorrect mediaType" },
	{ PM_ERR_BAD_PARAM_SYNTAX,		"programming: incorrect parameter syntax" },
	{ PM_ERR_BAD_PRINT_QUALITY,		"programming: incorrect printQuality" },
	{ PM_ERR_BAD_TRAY,			"programming: incorrect tray" },
	{ PM_ERR_INSET_OUTSIDE_AREA,		"programming: inset outside printable area" },
	{ PM_ERR_NO_FILES_TO_PRINT,		"programming: no files to print" },
	{ PM_ERR_OUT_OF_ORDER_CURRENTPAGE,	"programming: out of order currentpage" },
	{ PM_ERR_TEMP_FILE_TOO_LARGE,		"programming: temp file larger than specified" },
	{ PM_ERR_UNKNOWN_JOB_ID,		"programming: unknown job ID" },
	{ PM_ERR_UNSUPPORTED_MIME_TYPE,		"programming: unsupported or null mime type" },
	{ PM_ERR_UNSUPPORTED_PORT,		"programming: unsupported port" },
	{ PM_ERR_FILE_NOT_IN_ANY_JOB,		"programming: file not found in any job" },

	{ PM_ERR_FILECACHE_BAD_REPLY,		"system: bad reply from file cache" },
	{ PM_ERR_PRINTER_DB_ACCESS,		"system: can't access AddPrinter Database" },
	{ PM_ERR_JOB_ADD_PAGE_FAILED,		"system: could not add page" },
	{ PM_ERR_JOB_GET_FINAL_PARAMS_FAILED,	"system: could not get final job params" },
	{ PM_ERR_NO_FILECACHE_PATH,		"system: no path to file cache" },
	{ PM_ERR_NO_SUITABLE_PLUGIN,		"system:no suitable plugin" },
	{ PM_ERR_SUBSCRIPTION_FAILED,		"system: subscription-set-cancel failed" },
	{ PM_ERR_JOB_ADD_FAILED,		"system: unable to add job" },
	{ PM_ERR_JOB_CANCEL_FAILED,		"info: unable to cancel" },
	{ PM_ERR_JOB_FLAG_LAST_PAGE_FAILED,	"system:unable to flag last page" },
	{ PM_ERR_JOB_START_FAILED,		"system: unable to start job" },

	{ 0, NULL },
};

const char *pm_error_text(int code)
{
	int i;

	for (i = 0; error_texts[i].text; i++) {
		if (error_texts[i].code == code)
			return error_texts[i].text;
	}

	return "unknown: unknown job error";
}

// vim:ts=4:sw=4:noexpandtab
