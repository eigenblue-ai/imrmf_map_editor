// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

// C ABI of the desktop CRDT client. Free every char* with
// imrmf_client_string_free. Fallible calls return "OK:..." or "ERR:...".

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void imrmf_client_string_free(char *s);

// Takes the server's MountConfig, returns building ids one per line.
char *imrmf_client_mount(const char *server_url, const char *config_json);
char *imrmf_client_load_building(const char *server_url, const char *id);
char *imrmf_client_put_building(const char *server_url, const char *id,
                                const char *yaml);

char *imrmf_client_connect(const char *ws_url);
void imrmf_client_disconnect(void);
int imrmf_client_is_connected(void);
int imrmf_client_is_synced(void);

int imrmf_client_remote_dirty(void);
void imrmf_client_clear_remote_dirty(void);
char *imrmf_client_snapshot_yaml(void);
char *imrmf_client_push_yaml(const char *yaml);

#ifdef __cplusplus
} // extern "C"
#endif
