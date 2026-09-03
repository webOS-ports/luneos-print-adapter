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

#ifndef PRINT_ERRORS_H_
#define PRINT_ERRORS_H_

/*
 * The webOS 3.0.5 printmgr error codes, recovered from
 * error_code_to_text_trans_table in the shipped printmgrd binary and confirmed
 * against a live TouchPad. Clients in the wild - notably the enyo printdialog
 * lib's PrintManagerError.js - switch on these numbers directly, so the values
 * and the bands they sit in are API, not an implementation detail.
 *
 * Bands:  -200s user      -300s rendering   -400s communication
 *         -500s info      -600s programming -700s system
 */

#define PM_ERR_PRINTER_UNKNOWN_ID		-202
#define PM_ERR_PRINTER_NO_RESPONSE_MANUAL	-203
#define PM_ERR_PRINTER_DUPLICATE_ID		-204
#define PM_ERR_PRINTER_DUPLICATE_IP		-205
#define PM_ERR_PRINTER_IP_NOT_VALID		-206
#define PM_ERR_JOB_TEMP_FILE_NO_ROOM		-233
#define PM_ERR_PRINTER_NOT_SUPPORTED		-238
#define PM_ERR_JOB_NO_JOB_HANDLES		-243
#define PM_ERR_PRINTER_DUPLICATE_IP_ZERO	-245
#define PM_ERR_NO_WIFI_CONNECTION		-298
#define PM_ERR_JOB_UNKNOWN			-299

#define PM_ERR_COMM_FAILED			-401
#define PM_ERR_COMM_GET_CAPS_FAILED		-402
#define PM_ERR_COMM_NO_RESPONSE			-403
#define PM_ERR_COMM_ADDRESS_INVALID		-404
#define PM_ERR_COMM_NO_PING_RESPONSE		-405
#define PM_ERR_COMM_ADDRESS_UNKNOWN		-406

#define PM_ERR_JOB_ALREADY_CLOSED		-501
#define PM_ERR_JOB_CANCEL_REQUESTED		-502
#define PM_ERR_SET_CURRENT_PRINTER_FAILED	-503

#define PM_ERR_FILE_ZERO_LENGTH			-601
#define PM_ERR_INSETS_WITH_BORDERLESS		-602
#define PM_ERR_EMPTY_FILE_EXTENSION		-603
#define PM_ERR_FILE_DOES_NOT_EXIST		-604
#define PM_ERR_BAD_COLOR			-605
#define PM_ERR_BAD_DRYTIME			-606
#define PM_ERR_BAD_DUPLEX			-607
#define PM_ERR_BAD_MEDIA_SIZE			-608
#define PM_ERR_BAD_MEDIA_TYPE			-609
#define PM_ERR_BAD_PARAM_SYNTAX			-610
#define PM_ERR_BAD_PRINT_QUALITY		-611
#define PM_ERR_BAD_TRAY				-612
#define PM_ERR_INSET_OUTSIDE_AREA		-613
#define PM_ERR_NO_FILES_TO_PRINT		-614
#define PM_ERR_OUT_OF_ORDER_CURRENTPAGE		-615
#define PM_ERR_TEMP_FILE_TOO_LARGE		-616
#define PM_ERR_UNKNOWN_JOB_ID			-617
#define PM_ERR_UNSUPPORTED_MIME_TYPE		-618
#define PM_ERR_UNSUPPORTED_PORT			-619
#define PM_ERR_FILE_NOT_IN_ANY_JOB		-620

#define PM_ERR_FILECACHE_BAD_REPLY		-701
#define PM_ERR_PRINTER_DB_ACCESS		-702
#define PM_ERR_JOB_ADD_PAGE_FAILED		-703
#define PM_ERR_JOB_GET_FINAL_PARAMS_FAILED	-704
#define PM_ERR_NO_FILECACHE_PATH		-705
#define PM_ERR_NO_SUITABLE_PLUGIN		-706
#define PM_ERR_SUBSCRIPTION_FAILED		-707
#define PM_ERR_JOB_ADD_FAILED			-708
#define PM_ERR_JOB_CANCEL_FAILED		-709
#define PM_ERR_JOB_FLAG_LAST_PAGE_FAILED	-710
#define PM_ERR_JOB_START_FAILED			-711

const char *pm_error_text(int code);

#endif

// vim:ts=4:sw=4:noexpandtab
