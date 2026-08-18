// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

// C ABI of the desktop CRDT client. Free every char* with
// rmf_client_string_free. Fallible calls return "OK:..." or "ERR:...".

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void rmf_client_string_free(char *s);

// Takes the server's MountConfig, returns building ids one per line.
char *rmf_client_fetch_config(const char *server_url);
char *rmf_client_list_buildings(const char *server_url);
char *rmf_client_mount(const char *server_url, const char *config_json);
char *rmf_client_load_building(const char *server_url, const char *id);
char *rmf_client_put_building(const char *server_url, const char *id,
                              const char *yaml);
// One image, by the yaml-relative path the map refers to it by.
char *rmf_client_put_asset(const char *server_url, const char *id,
                           const char *path, const unsigned char *data,
                           size_t len);

char *rmf_client_connect(const char *ws_url);
// A doc with no server behind it, so a map opened from a file still has undo.
char *rmf_client_start_local_session(const char *yaml);
void rmf_client_disconnect(void);
int rmf_client_is_connected(void);
int rmf_client_is_synced(void);

// Undo/redo over this client's own edits. Return 1 if anything changed.
int rmf_client_undo(void);
int rmf_client_redo(void);
int rmf_client_can_undo(void);
int rmf_client_can_redo(void);

int rmf_client_remote_dirty(void);
void rmf_client_clear_remote_dirty(void);
char *rmf_client_snapshot_yaml(void);
char *rmf_client_push_yaml(const char *yaml);

#ifdef __cplusplus
} // extern "C"
#endif
