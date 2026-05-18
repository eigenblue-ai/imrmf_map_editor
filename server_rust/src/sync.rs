// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

// Yjs sync protocol, wire-compatible with the y-websocket JS provider.
// Spec: https://github.com/yjs/y-protocols/blob/master/sync.js

use anyhow::{anyhow, Result};
use yrs::updates::decoder::Decode;
use yrs::updates::encoder::Encode;
use yrs::{ReadTxn, StateVector, Transact, Update};

pub const MSG_SYNC: u64 = 0;
pub const MSG_AWARENESS: u64 = 1;

pub const SYNC_STEP_1: u64 = 0;
pub const SYNC_STEP_2: u64 = 1;
pub const SYNC_UPDATE: u64 = 2;

/// Write a varint (LEB128 unsigned) to `out`.
fn write_varint(out: &mut Vec<u8>, mut value: u64) {
    while value >= 0x80 {
        out.push(((value & 0x7f) as u8) | 0x80);
        value >>= 7;
    }
    out.push((value & 0x7f) as u8);
}

/// Read a varint from `buf` starting at `*pos`; advances `*pos` past it.
fn read_varint(buf: &[u8], pos: &mut usize) -> Result<u64> {
    let mut result: u64 = 0;
    let mut shift = 0;
    loop {
        if *pos >= buf.len() {
            return Err(anyhow!("varint truncated"));
        }
        let byte = buf[*pos];
        *pos += 1;
        result |= ((byte & 0x7f) as u64) << shift;
        if byte & 0x80 == 0 {
            return Ok(result);
        }
        shift += 7;
        if shift >= 64 {
            return Err(anyhow!("varint overflow"));
        }
    }
}

/// Read a varint-prefixed byte slice (length-prefixed).
fn read_varint_buffer<'a>(buf: &'a [u8], pos: &mut usize) -> Result<&'a [u8]> {
    let len = read_varint(buf, pos)? as usize;
    if *pos + len > buf.len() {
        return Err(anyhow!("buffer truncated"));
    }
    let out = &buf[*pos..*pos + len];
    *pos += len;
    Ok(out)
}

fn write_varint_buffer(out: &mut Vec<u8>, bytes: &[u8]) {
    write_varint(out, bytes.len() as u64);
    out.extend_from_slice(bytes);
}

/// What the protocol layer wants the caller to do after `handle_message`.
pub struct HandleResult {
    /// Bytes to send back to the requesting client (e.g. SyncStep2 response).
    pub reply: Option<Vec<u8>>,
    /// If a remote update was applied to the local doc, the encoded update is
    /// returned here so the caller can broadcast it to other peers.
    pub broadcast_update: Option<Vec<u8>>,
}

/// Decode a single incoming WebSocket binary message and apply it to `doc`.
pub fn handle_message(doc: &yrs::Doc, msg: &[u8]) -> Result<HandleResult> {
    let mut pos = 0;
    let mut reply: Option<Vec<u8>> = None;
    let mut broadcast_update: Option<Vec<u8>> = None;

    while pos < msg.len() {
        let msg_type = read_varint(msg, &mut pos)?;
        match msg_type {
            x if x == MSG_SYNC => {
                let sub = read_varint(msg, &mut pos)?;
                match sub {
                    s if s == SYNC_STEP_1 => {
                        let sv_bytes = read_varint_buffer(msg, &mut pos)?;
                        let sv = StateVector::decode_v1(sv_bytes)
                            .map_err(|e| anyhow!("decode state vector: {e}"))?;
                        let update_bytes = {
                            let txn = doc.transact();
                            txn.encode_state_as_update_v1(&sv)
                        };
                        let mut out = Vec::with_capacity(update_bytes.len() + 8);
                        write_varint(&mut out, MSG_SYNC);
                        write_varint(&mut out, SYNC_STEP_2);
                        write_varint_buffer(&mut out, &update_bytes);
                        reply = Some(merge_reply(reply, out));
                    }
                    s if s == SYNC_STEP_2 || s == SYNC_UPDATE => {
                        let update_bytes = read_varint_buffer(msg, &mut pos)?;
                        let update = Update::decode_v1(update_bytes)
                            .map_err(|e| anyhow!("decode update: {e}"))?;
                        {
                            let mut txn = doc.transact_mut();
                            txn.apply_update(update)
                                .map_err(|e| anyhow!("apply update: {e}"))?;
                        }
                        // Re-encode for broadcast (preserves origin separation).
                        if broadcast_update.is_none() {
                            broadcast_update = Some(update_bytes.to_vec());
                        }
                    }
                    other => {
                        tracing::warn!("unknown sync sub-type: {}", other);
                        break;
                    }
                }
            }
            x if x == MSG_AWARENESS => {
                // Awareness/presence is forwarded opaquely; we don't use it.
                let _ = read_varint_buffer(msg, &mut pos)?;
            }
            other => {
                tracing::warn!("unknown message type: {}", other);
                break;
            }
        }
    }
    Ok(HandleResult {
        reply,
        broadcast_update,
    })
}

fn merge_reply(existing: Option<Vec<u8>>, mut new: Vec<u8>) -> Vec<u8> {
    if let Some(mut prev) = existing {
        prev.append(&mut new);
        prev
    } else {
        new
    }
}

/// Encode a SyncStep1 message containing this doc's current state vector.
/// Sent eagerly to a new connection so it knows which updates it lacks.
pub fn encode_sync_step1(doc: &yrs::Doc) -> Vec<u8> {
    let sv = doc.transact().state_vector();
    let sv_bytes = sv.encode_v1();
    let mut out = Vec::with_capacity(sv_bytes.len() + 8);
    write_varint(&mut out, MSG_SYNC);
    write_varint(&mut out, SYNC_STEP_1);
    write_varint_buffer(&mut out, &sv_bytes);
    out
}

/// Wrap a raw update payload as a sync `Update` message ready to broadcast.
pub fn encode_update_message(update: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(update.len() + 8);
    write_varint(&mut out, MSG_SYNC);
    write_varint(&mut out, SYNC_UPDATE);
    write_varint_buffer(&mut out, update);
    out
}
