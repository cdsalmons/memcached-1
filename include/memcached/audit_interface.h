/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef MEMCACHED_AUDIT_INTERFACE_H
#define MEMCACHED_AUDIT_INTERFACE_H

#include <memcached/extension.h>
#include <memcached/visibility.h>
#include <platform/platform.h>

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

/*
 * Response codes for audit operations.
 */

typedef enum {
    /* The command executed successfully */
    AUDIT_SUCCESS = 0x00,
    /* A fatal error occurred, and the server should shut down as soon
     * as possible
     */
    AUDIT_FATAL   = 0x01,
    /* Generic failure. */
    AUDIT_FAILED  = 0x02,
    /* performing configuration would block */
    AUDIT_EWOULDBLOCK  = 0x03
} AUDIT_ERROR_CODE;


typedef struct {
    uint8_t version;
    uint32_t min_file_rotation_time;
    uint32_t max_file_rotation_time;
    EXTENSION_LOGGER_DESCRIPTOR *log_extension;
    void (*notify_io_complete)(const void *cookie,
                               ENGINE_ERROR_CODE status);
}AUDIT_EXTENSION_DATA;


MEMCACHED_PUBLIC_API
AUDIT_ERROR_CODE start_auditdaemon(const AUDIT_EXTENSION_DATA *extension_data);

MEMCACHED_PUBLIC_API
AUDIT_ERROR_CODE configure_auditdaemon(const char *config, const void *cookie);

MEMCACHED_PUBLIC_API
AUDIT_ERROR_CODE put_audit_event(const uint32_t audit_eventid, const void *payload, size_t length);

/**
 * Put a JSON object into the audit log (and add timestamp if missing)
 *
 * @param id the audit identifier
 * @param event the actual event data
 * @return AUDIT_SUCCESS upon success, AUDIT_FAILURE otherwise
 */
MEMCACHED_PUBLIC_API
AUDIT_ERROR_CODE put_json_audit_event(uint32_t audit_eventid, cJSON *root);

MEMCACHED_PUBLIC_API
AUDIT_ERROR_CODE shutdown_auditdaemon(void);

MEMCACHED_PUBLIC_API
void process_auditd_stats(ADD_STAT add_stats, void *c);

#ifdef __cplusplus
}

// The following API is used by tests in order to test the
// internals. It should not be used elsewhere since it may
// affect performance and the behavior

MEMCACHED_PUBLIC_API
void audit_test_timetravel(time_t offset);

MEMCACHED_PUBLIC_API
std::string audit_generate_timestamp(void);

MEMCACHED_PUBLIC_API
void audit_set_audit_processed_listener(void (*listener)(void));

#endif

#endif /* MEMCACHED_AUDIT_INTERFACE_H */
