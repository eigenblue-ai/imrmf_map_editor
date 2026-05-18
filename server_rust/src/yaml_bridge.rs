// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::sync::Arc;

use anyhow::{anyhow, Result};
use yrs::{
    Any, Array, ArrayPrelim, ArrayRef, Doc, GetString, Map, MapPrelim, MapRef, Out, ReadTxn,
    Transact, TransactionMut,
};

const ROOT_KEY: &str = "building";

pub fn seed_doc(doc: &Doc, yaml_text: &str) -> Result<()> {
    let root = doc.get_or_insert_map(ROOT_KEY);
    let mut txn = doc.transact_mut();

    let keys: Vec<String> = root.iter(&txn).map(|(k, _)| k.to_string()).collect();
    for k in keys {
        root.remove(&mut txn, &k);
    }
    if yaml_text.trim().is_empty() {
        return Ok(());
    }

    let value: serde_yaml::Value = serde_yaml::from_str(yaml_text)?;
    match value {
        serde_yaml::Value::Null => Ok(()),
        serde_yaml::Value::Mapping(map) => {
            for (k, v) in map {
                let Some(key) = k.as_str() else { continue };
                insert_value_into_map(&mut txn, &root, key, &v)?;
            }
            Ok(())
        }
        _ => Err(anyhow!("building.yaml root must be a mapping")),
    }
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
    let mut out = serde_yaml::Mapping::new();
    for (k, v) in map.iter(txn) {
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
}
