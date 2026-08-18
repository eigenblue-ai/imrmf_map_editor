// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::sync::Arc;

use anyhow::{anyhow, Result};
use yrs::{
    Any, Array, ArrayPrelim, ArrayRef, Doc, GetString, Map, MapPrelim, MapRef, Out, ReadTxn,
    Transact, TransactionMut,
};

pub const ROOT_KEY: &str = "building";

pub fn seed_doc(doc: &Doc, yaml_text: &str) -> Result<()> {
    let root = doc.get_or_insert_map(ROOT_KEY);
    let mut txn = doc.transact_mut();
    seed_in_txn(&root, &mut txn, yaml_text)
}

/// Same, but tagging the transaction with an origin. The desktop client uses it
/// so its UndoManager can tell local edits from ones arriving over sync.
pub fn seed_doc_with_origin<O: Into<yrs::Origin>>(
    doc: &Doc,
    yaml_text: &str,
    origin: O,
) -> Result<()> {
    let root = doc.get_or_insert_map(ROOT_KEY);
    let mut txn = doc.transact_mut_with(origin);
    seed_in_txn(&root, &mut txn, yaml_text)
}

fn seed_in_txn(root: &yrs::MapRef, txn: &mut yrs::TransactionMut, yaml_text: &str) -> Result<()> {
    if yaml_text.trim().is_empty() {
        clear_map(txn, root);
        return Ok(());
    }

    let value: serde_yaml::Value = serde_yaml::from_str(yaml_text)?;
    match value {
        serde_yaml::Value::Null => {
            clear_map(txn, root);
            Ok(())
        }
        serde_yaml::Value::Mapping(map) => merge_mapping(txn, root, &map),
        _ => Err(anyhow!("building.yaml root must be a mapping")),
    }
}

fn clear_map(txn: &mut TransactionMut, map: &MapRef) {
    let keys: Vec<String> = map.iter(txn).map(|(k, _)| k.to_string()).collect();
    for k in keys {
        map.remove(txn, &k);
    }
}

/// Reconciles the doc against `src` in place rather than dropping every key and
/// rebuilding. An editor pushes the whole map back after each edit, so a rebuild
/// made one moved vertex cost a delete and reinsert of the entire building, and
/// an update the size of the map. Merging touches only what changed.
fn merge_mapping(txn: &mut TransactionMut, map: &MapRef, src: &serde_yaml::Mapping) -> Result<()> {
    let stale: Vec<String> = map
        .iter(txn)
        .map(|(k, _)| k.to_string())
        .filter(|k| !src.contains_key(serde_yaml::Value::String(k.clone())))
        .collect();
    for k in stale {
        map.remove(txn, &k);
    }
    for (k, v) in src {
        let Some(key) = k.as_str() else { continue };
        merge_into_map(txn, map, key, v)?;
    }
    Ok(())
}

fn merge_into_map(
    txn: &mut TransactionMut,
    map: &MapRef,
    key: &str,
    value: &serde_yaml::Value,
) -> Result<()> {
    match (map.get(txn, key), value) {
        (Some(Out::YMap(existing)), serde_yaml::Value::Mapping(sub)) => {
            merge_mapping(txn, &existing, sub)
        }
        (Some(Out::YArray(existing)), serde_yaml::Value::Sequence(seq)) => {
            merge_array(txn, &existing, seq)
        }
        (Some(Out::Any(current)), scalar) if !is_container(scalar) => {
            let any = yaml_scalar_to_any(scalar);
            if current != any {
                map.insert(txn, key, any);
            }
            Ok(())
        }
        // Absent, or the shape changed: write it fresh.
        _ => insert_value_into_map(txn, map, key, value),
    }
}

/// Positional reconcile: entry i merges against entry i, and the tail is pushed
/// or trimmed. Vertices and lanes are appended, so an added row costs one push.
/// A row dropped from the middle rewrites the rows after it, still far less than
/// rewriting the array.
fn merge_array(txn: &mut TransactionMut, arr: &ArrayRef, seq: &[serde_yaml::Value]) -> Result<()> {
    let have = arr.len(txn) as usize;
    for (i, v) in seq.iter().enumerate() {
        if i >= have {
            push_value_into_array(txn, arr, v)?;
            continue;
        }
        let idx = i as u32;
        match (arr.get(txn, idx), v) {
            (Some(Out::YMap(existing)), serde_yaml::Value::Mapping(sub)) => {
                merge_mapping(txn, &existing, sub)?
            }
            (Some(Out::YArray(existing)), serde_yaml::Value::Sequence(sub)) => {
                merge_array(txn, &existing, sub)?
            }
            (Some(Out::Any(current)), scalar) if !is_container(scalar) => {
                let any = yaml_scalar_to_any(scalar);
                if current != any {
                    arr.remove(txn, idx);
                    arr.insert(txn, idx, any);
                }
            }
            // Absent, or the shape changed: write it fresh.
            _ => {
                arr.remove(txn, idx);
                insert_value_into_array(txn, arr, idx, v)?;
            }
        }
    }
    if have > seq.len() {
        arr.remove_range(txn, seq.len() as u32, (have - seq.len()) as u32);
    }
    Ok(())
}

fn is_container(value: &serde_yaml::Value) -> bool {
    matches!(
        value,
        serde_yaml::Value::Mapping(_) | serde_yaml::Value::Sequence(_)
    )
}

pub fn serialize_doc<T: ReadTxn>(txn: &T) -> Result<String> {
    let root = txn
        .get_map(ROOT_KEY)
        .ok_or_else(|| anyhow!("no '{}' map in doc", ROOT_KEY))?;
    let value = read_map(txn, &root)?;
    let yaml = serde_yaml::to_string(&value)?;
    Ok(yaml)
}

fn insert_value_into_map(
    txn: &mut TransactionMut,
    map: &MapRef,
    key: &str,
    value: &serde_yaml::Value,
) -> Result<()> {
    match value {
        serde_yaml::Value::Mapping(sub) => {
            let inserted: MapRef = map.insert(txn, key, MapPrelim::default());
            for (k, v) in sub {
                let sub_key = match k.as_str() {
                    Some(s) => s.to_string(),
                    None => continue,
                };
                insert_value_into_map(txn, &inserted, &sub_key, v)?;
            }
        }
        serde_yaml::Value::Sequence(seq) => {
            let inserted: ArrayRef = map.insert(txn, key, ArrayPrelim::default());
            for v in seq {
                push_value_into_array(txn, &inserted, v)?;
            }
        }
        scalar => {
            map.insert(txn, key, yaml_scalar_to_any(scalar));
        }
    }
    Ok(())
}

fn push_value_into_array(
    txn: &mut TransactionMut,
    arr: &ArrayRef,
    value: &serde_yaml::Value,
) -> Result<()> {
    match value {
        serde_yaml::Value::Mapping(sub) => {
            let inserted: MapRef = arr.push_back(txn, MapPrelim::default());
            for (k, v) in sub {
                let sub_key = match k.as_str() {
                    Some(s) => s.to_string(),
                    None => continue,
                };
                insert_value_into_map(txn, &inserted, &sub_key, v)?;
            }
        }
        serde_yaml::Value::Sequence(seq) => {
            let inserted: ArrayRef = arr.push_back(txn, ArrayPrelim::default());
            for v in seq {
                push_value_into_array(txn, &inserted, v)?;
            }
        }
        scalar => {
            arr.push_back(txn, yaml_scalar_to_any(scalar));
        }
    }
    Ok(())
}

fn insert_value_into_array(
    txn: &mut TransactionMut,
    arr: &ArrayRef,
    index: u32,
    value: &serde_yaml::Value,
) -> Result<()> {
    match value {
        serde_yaml::Value::Mapping(sub) => {
            let inserted: MapRef = arr.insert(txn, index, MapPrelim::default());
            for (k, v) in sub {
                let Some(sub_key) = k.as_str() else { continue };
                insert_value_into_map(txn, &inserted, sub_key, v)?;
            }
        }
        serde_yaml::Value::Sequence(seq) => {
            let inserted: ArrayRef = arr.insert(txn, index, ArrayPrelim::default());
            for v in seq {
                push_value_into_array(txn, &inserted, v)?;
            }
        }
        scalar => {
            arr.insert(txn, index, yaml_scalar_to_any(scalar));
        }
    }
    Ok(())
}

fn yaml_scalar_to_any(value: &serde_yaml::Value) -> Any {
    match value {
        serde_yaml::Value::Null => Any::Null,
        serde_yaml::Value::Bool(b) => Any::Bool(*b),
        serde_yaml::Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                Any::BigInt(i)
            } else if let Some(u) = n.as_u64() {
                Any::BigInt(u as i64)
            } else {
                Any::Number(n.as_f64().unwrap_or(0.0))
            }
        }
        serde_yaml::Value::String(s) => Any::String(Arc::<str>::from(s.as_str())),
        // Tagged / sequences / mappings shouldn't reach here.
        _ => Any::Null,
    }
}

fn any_to_yaml(any: &Any) -> serde_yaml::Value {
    match any {
        Any::Null => serde_yaml::Value::Null,
        Any::Undefined => serde_yaml::Value::Null,
        Any::Bool(b) => serde_yaml::Value::Bool(*b),
        Any::Number(n) => {
            // Keep ints as ints so vertex indices don't drift to floats.
            if n.fract() == 0.0 && n.is_finite() && n.abs() < 1e18 {
                serde_yaml::Value::Number((*n as i64).into())
            } else {
                serde_yaml::Value::Number(serde_yaml::Number::from(*n))
            }
        }
        Any::BigInt(i) => serde_yaml::Value::Number((*i).into()),
        Any::String(s) => serde_yaml::Value::String(s.to_string()),
        Any::Buffer(_) => serde_yaml::Value::Null,
        Any::Array(items) => {
            let seq: Vec<_> = items.iter().map(any_to_yaml).collect();
            serde_yaml::Value::Sequence(seq)
        }
        Any::Map(entries) => {
            let mut m = serde_yaml::Mapping::new();
            for (k, v) in entries.iter() {
                m.insert(serde_yaml::Value::String(k.to_string()), any_to_yaml(v));
            }
            serde_yaml::Value::Mapping(m)
        }
    }
}

fn read_value<T: ReadTxn>(txn: &T, value: &Out) -> Result<serde_yaml::Value> {
    Ok(match value {
        Out::Any(a) => any_to_yaml(a),
        Out::YMap(m) => read_map(txn, m)?,
        Out::YArray(a) => read_array(txn, a)?,
        Out::YText(t) => serde_yaml::Value::String(t.get_string(txn)),
        // YXmlElement / YXmlText / YDoc shouldn't appear in our schema.
        _ => serde_yaml::Value::Null,
    })
}

fn read_map<T: ReadTxn>(txn: &T, map: &MapRef) -> Result<serde_yaml::Value> {
    // Sorted, because a yrs map branch is a std HashMap whose iteration order is
    // seeded per process and reshuffles as the map is mutated. Unsorted, the
    // same document serialises differently in every client, which moves `levels`
    // around under anything tracking a level by position.
    let mut keys: Vec<&str> = map.iter(txn).map(|(k, _)| k).collect();
    keys.sort_unstable();

    let mut out = serde_yaml::Mapping::new();
    for k in keys {
        let Some(v) = map.get(txn, k) else { continue };
        out.insert(
            serde_yaml::Value::String(k.to_string()),
            read_value(txn, &v)?,
        );
    }
    Ok(serde_yaml::Value::Mapping(out))
}

fn read_array<T: ReadTxn>(txn: &T, arr: &ArrayRef) -> Result<serde_yaml::Value> {
    let mut out: Vec<serde_yaml::Value> = Vec::with_capacity(arr.len(txn) as usize);
    for v in arr.iter(txn) {
        out.push(read_value(txn, &v)?);
    }
    Ok(serde_yaml::Value::Sequence(out))
}

#[cfg(test)]
mod tests {
    use super::*;
    use yrs::Transact;

    #[test]
    fn round_trips_simple_yaml() {
        let yaml = r#"
name: example
coordinate_system: reference_image
levels:
  L1:
    elevation: 0
    drawing:
      filename: floorplan.png
    vertices:
      - [491.464, 172.519, 0, ""]
      - [802.78, 1808.78, 0, "charging_station", {dock_name: [1, charging], is_charger: [4, true]}]
    lanes:
      - [0, 1, {bidirectional: [4, false], graph_idx: [2, 0]}]
"#;
        let doc = Doc::new();
        seed_doc(&doc, yaml).unwrap();
        let out = {
            let txn = doc.transact();
            serialize_doc(&txn).unwrap()
        };
        // Re-parse and compare structurally to avoid whitespace pedantry.
        let original: serde_yaml::Value = serde_yaml::from_str(yaml).unwrap();
        let echoed: serde_yaml::Value = serde_yaml::from_str(&out).unwrap();
        assert_eq!(original, echoed);
    }

    fn dump(doc: &Doc) -> serde_yaml::Value {
        let txn = doc.transact();
        serde_yaml::from_str(&serialize_doc(&txn).unwrap()).unwrap()
    }

    const BASE: &str = r#"
name: example
levels:
  L1:
    elevation: 0
    drawing:
      filename: floorplan.png
    vertices:
      - [1, 2, 0, "a"]
      - [3, 4, 0, "b"]
    lanes:
      - [0, 1, {bidirectional: [4, false]}]
"#;

    #[test]
    fn reseeding_reaches_the_new_state() {
        // Every shape of change the editor can push: a scalar edited, a row
        // appended, a row dropped, a key removed, and a key whose type flipped.
        let cases = [
            BASE.replace("[1, 2, 0, \"a\"]", "[9, 2, 0, \"a\"]"),
            BASE.replace(
                "      - [3, 4, 0, \"b\"]\n",
                "      - [3, 4, 0, \"b\"]\n      - [5, 6, 0, \"c\"]\n",
            ),
            BASE.replace("      - [3, 4, 0, \"b\"]\n", ""),
            BASE.replace("    drawing:\n      filename: floorplan.png\n", ""),
            BASE.replace("    elevation: 0", "    elevation: [1, 2]"),
            BASE.replace("elevation: 0", "elevation: 0\n    layers: []"),
        ];
        for next in cases {
            let doc = Doc::new();
            seed_doc(&doc, BASE).unwrap();
            seed_doc(&doc, &next).unwrap();
            let want: serde_yaml::Value = serde_yaml::from_str(&next).unwrap();
            assert_eq!(dump(&doc), want, "after reseeding with:\n{next}");
        }
    }

    #[test]
    fn an_unchanged_reseed_is_a_no_op() {
        let doc = Doc::new();
        seed_doc(&doc, BASE).unwrap();
        let before = doc.transact().state_vector();
        seed_doc(&doc, BASE).unwrap();
        // An update carrying nothing is still a two byte envelope.
        let update = doc.transact().encode_state_as_update_v1(&before);
        assert!(update.len() <= 2, "reseed emitted {} bytes", update.len());
    }

    // Two clients on one document, each pushing the whole yaml back after its
    // own edit, as the desktop client does. A reseed that rebuilt the tree made
    // each push delete what the other peer had just written. Merging in place
    // keeps both edits and leaves the two docs identical.
    #[test]
    fn concurrent_pushes_from_two_peers_both_survive() {
        use yrs::updates::decoder::Decode;
        use yrs::Update;

        let a = Doc::new();
        seed_doc(&a, BASE).unwrap();
        let b = Doc::new();
        {
            let update = a.transact().encode_state_as_update_v1(&Default::default());
            let mut txn = b.transact_mut();
            txn.apply_update(Update::decode_v1(&update).unwrap())
                .unwrap();
        }

        // A renames the first vertex, B the second, neither having seen the
        // other's edit yet.
        let sv_a = a.transact().state_vector();
        seed_doc(&a, &BASE.replace("\"a\"", "\"a2\"")).unwrap();
        let from_a = a.transact().encode_state_as_update_v1(&sv_a);

        let sv_b = b.transact().state_vector();
        seed_doc(&b, &BASE.replace("\"b\"", "\"b2\"")).unwrap();
        let from_b = b.transact().encode_state_as_update_v1(&sv_b);

        b.transact_mut()
            .apply_update(Update::decode_v1(&from_a).unwrap())
            .unwrap();
        a.transact_mut()
            .apply_update(Update::decode_v1(&from_b).unwrap())
            .unwrap();

        assert_eq!(dump(&a), dump(&b), "peers diverged");
        let merged = serde_yaml::to_string(&dump(&a)).unwrap();
        assert!(merged.contains("a2"), "lost A's edit:\n{merged}");
        assert!(merged.contains("b2"), "lost B's edit:\n{merged}");
    }

    #[test]
    fn one_edit_costs_far_less_than_the_map() {
        // A vertex moved on a map of 500 used to resend the whole document.
        let map = |bump: i32| {
            let mut y = String::from("name: b\nlevels:\n  L1:\n    elevation: 0\n    vertices:\n");
            for i in 0..500 {
                let x = if i == 0 { bump } else { i };
                y.push_str(&format!("      - [{x}, {i}, 0, \"v{i}\"]\n"));
            }
            y
        };
        let doc = Doc::new();
        seed_doc(&doc, &map(0)).unwrap();
        let whole = doc
            .transact()
            .encode_state_as_update_v1(&Default::default());
        let before = doc.transact().state_vector();
        seed_doc(&doc, &map(7)).unwrap();
        let update = doc.transact().encode_state_as_update_v1(&before);
        assert!(
            update.len() * 50 < whole.len(),
            "one moved vertex sent {}B against a {}B map",
            update.len(),
            whole.len()
        );
    }
}
