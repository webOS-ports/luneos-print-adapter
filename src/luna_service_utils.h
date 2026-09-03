/* @@@LICENSE
*
* Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
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

#ifndef LUNA_SERVICE_UTILS_H_
#define LUNA_SERVICE_UTILS_H_

#include <luna-service2/lunaservice.h>
#include <pbnjson.h>

/*
 * printmgr's error replies carry a numeric errorCode as well as errorText, and
 * clients switch on the number, so the generic helpers here always emit both.
 */
void luna_service_message_reply_error_code(LSHandle *handle, LSMessage *message,
                                           int error_code);
void luna_service_message_reply_custom_error(LSHandle *handle, LSMessage *message,
                                             const char *error_text);
void luna_service_message_reply_error_bad_json(LSHandle *handle, LSMessage *message);
void luna_service_message_reply_error_invalid_params(LSHandle *handle, LSMessage *message);
void luna_service_message_reply_error_internal(LSHandle *handle, LSMessage *message);
void luna_service_message_reply_success(LSHandle *handle, LSMessage *message);

jvalue_ref luna_service_message_parse_and_validate(const char *payload);
bool luna_service_message_validate_and_send(LSHandle *handle, LSMessage *message,
                                            jvalue_ref reply_obj);
bool luna_service_check_for_subscription_and_process(LSHandle *handle, LSMessage *message);
void luna_service_post_subscription(LSHandle *handle, const char *path,
                                    const char *method, jvalue_ref reply_obj);

/* Small typed accessors; all return false when absent or the wrong type. */
bool luna_get_string(jvalue_ref obj, const char *key, const char **out);
bool luna_get_int(jvalue_ref obj, const char *key, int *out);
bool luna_get_double(jvalue_ref obj, const char *key, double *out);
bool luna_get_bool(jvalue_ref obj, const char *key, bool *out);
bool luna_has_key(jvalue_ref obj, const char *key);

#endif

// vim:ts=4:sw=4:noexpandtab
