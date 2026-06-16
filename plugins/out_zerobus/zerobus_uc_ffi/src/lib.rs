// Companion FFI for the Fluent Bit out_zerobus plugin.
//
// The C plugin can already create a Zerobus stream from a serialized protobuf
// `DescriptorProto` and ingest pre-encoded protobuf records (see zerobus.h:
// `zerobus_sdk_create_stream` / `zerobus_stream_ingest_proto_records`). What the
// C FFI does *not* expose are the two pieces the Vector sink gets "for free" from
// the SDK's Rust API + prost_reflect:
//
//   1. Turning a Unity Catalog table schema into a `DescriptorProto`
//      (the SDK's `schema::descriptor_from_uc_schema`).
//   2. Encoding an individual record into protobuf wire bytes that match that
//      descriptor (Vector uses a `prost_reflect::DynamicMessage`).
//
// This crate is a thin, C-callable bridge over exactly those two operations. It
// links against the SDK *as a library* (same as Vector does) — it does not modify
// the SDK. All data crossing the FFI boundary is plain bytes / C strings; no Rust
// objects are shared with the other (prebuilt libzerobus_ffi.a) Rust world.

use std::os::raw::{c_char, c_int};

use databricks_zerobus_ingest_sdk::schema::{descriptor_from_uc_schema, UcTableSchema};
use prost::Message;
use prost_reflect::prost_types::{DescriptorProto, FileDescriptorProto, FileDescriptorSet};
use prost_reflect::{DescriptorPool, DeserializeOptions, DynamicMessage, MessageDescriptor};

/// Opaque, C-owned handle holding everything needed to (a) hand the descriptor
/// bytes to the Zerobus SDK at stream-creation time and (b) encode records.
pub struct ZbUcSchema {
    /// Serialized `DescriptorProto` — exactly what
    /// `zerobus_sdk_create_stream(descriptor_proto_bytes, ...)` expects.
    descriptor_bytes: Vec<u8>,
    /// Resolved message descriptor used to build a `DynamicMessage` per record.
    message: MessageDescriptor,
}

/// Synthetic protobuf package for the generated file. It does not affect the
/// record wire format (only field numbers/types matter, and those come straight
/// from `descriptor_bytes`, which both this encoder and the server use), so any
/// valid identifier works — it just has to be consistent within this crate.
const SYNTHETIC_PACKAGE: &str = "zerobus";

/// Write `msg` into `*err_out` as an owned C string (caller frees with
/// `zb_uc_free_err`). No-op when `err_out` is null.
///
/// # Safety
/// `err_out`, when non-null, must be a valid pointer to a writable `*mut c_char`.
unsafe fn set_err(err_out: *mut *mut c_char, msg: String) {
    if err_out.is_null() {
        return;
    }
    // Truncate at the first interior NUL so CString::new never fails.
    let bytes = msg.into_bytes();
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    let mut v = bytes[..end].to_vec();
    v.push(0);
    // SAFETY: v is NUL-terminated and contains no interior NULs.
    let cstr = std::ffi::CString::from_vec_with_nul(v).unwrap_or_default();
    *err_out = cstr.into_raw();
}

/// Build a schema handle from the raw Unity Catalog table-metadata JSON (the
/// body of `GET /api/2.1/unity-catalog/tables/{name}`).
///
/// On success returns 0, sets `*out_handle`, and points `*out_desc_bytes` /
/// `*out_desc_len` at the serialized `DescriptorProto` owned by the handle (valid
/// until `zb_uc_schema_free`). On failure returns -1 and, if `out_err` is
/// non-null, sets `*out_err` to an owned error string.
///
/// # Safety
/// All non-null pointers must be valid; `table_json` must be a NUL-terminated
/// C string. `out_handle` must be non-null.
#[no_mangle]
pub unsafe extern "C" fn zb_uc_schema_from_table_json(
    table_json: *const c_char,
    out_handle: *mut *mut ZbUcSchema,
    out_desc_bytes: *mut *const u8,
    out_desc_len: *mut usize,
    out_err: *mut *mut c_char,
) -> c_int {
    if table_json.is_null() || out_handle.is_null() {
        set_err(out_err, "null argument".into());
        return -1;
    }

    let json = match std::ffi::CStr::from_ptr(table_json).to_str() {
        Ok(s) => s,
        Err(e) => {
            set_err(out_err, format!("table JSON is not valid UTF-8: {e}"));
            return -1;
        }
    };

    // `UcTableSchema` mirrors the UC REST response and ignores unknown fields, so
    // the raw table-metadata body deserializes directly.
    let schema: UcTableSchema = match serde_json::from_str(json) {
        Ok(s) => s,
        Err(e) => {
            set_err(out_err, format!("failed to parse Unity Catalog schema: {e}"));
            return -1;
        }
    };

    let descriptor = match descriptor_from_uc_schema(&schema) {
        Ok(d) => d,
        Err(e) => {
            set_err(out_err, format!("failed to build descriptor from schema: {e}"));
            return -1;
        }
    };

    // Re-decode the bytes into prost_reflect's prost_types view. This is the
    // single source of truth that keeps the descriptor handed to the SDK and the
    // descriptor used for encoding byte-for-byte identical, and it is robust even
    // if the SDK's prost-types and ours ever diverge (the wire format does not).
    let descriptor_bytes = descriptor.encode_to_vec();
    let reflect_descriptor = match DescriptorProto::decode(descriptor_bytes.as_slice()) {
        Ok(d) => d,
        Err(e) => {
            set_err(out_err, format!("failed to re-decode descriptor: {e}"));
            return -1;
        }
    };

    let message_name = reflect_descriptor.name().to_string();
    let file = FileDescriptorProto {
        name: Some(format!("{message_name}.proto")),
        package: Some(SYNTHETIC_PACKAGE.to_string()),
        message_type: vec![reflect_descriptor],
        ..Default::default()
    };
    let file_set = FileDescriptorSet { file: vec![file] };

    let pool = match DescriptorPool::from_file_descriptor_set(file_set) {
        Ok(p) => p,
        Err(e) => {
            set_err(out_err, format!("failed to build descriptor pool: {e}"));
            return -1;
        }
    };

    let full_name = format!("{SYNTHETIC_PACKAGE}.{message_name}");
    let message = match pool.get_message_by_name(&full_name) {
        Some(m) => m,
        None => {
            set_err(out_err, format!("message '{full_name}' not found in pool"));
            return -1;
        }
    };

    let handle = Box::new(ZbUcSchema {
        descriptor_bytes,
        message,
    });

    if !out_desc_bytes.is_null() {
        *out_desc_bytes = handle.descriptor_bytes.as_ptr();
    }
    if !out_desc_len.is_null() {
        *out_desc_len = handle.descriptor_bytes.len();
    }
    *out_handle = Box::into_raw(handle);
    0
}

/// Encode one JSON record into protobuf wire bytes matching the handle's
/// descriptor.
///
/// The record JSON is interpreted with the protobuf canonical JSON mapping
/// (prost_reflect): object keys match the table column names, and DATE /
/// TIMESTAMP columns must already carry integers (days / microseconds since the
/// Unix epoch) — see the SDK's schema module for the encoding contract. Unknown
/// keys are ignored so upstream metadata fields do not fail the encode.
///
/// On success returns 0 and sets `*out_bytes` / `*out_len` to a heap buffer the
/// caller must release with `zb_uc_free_bytes`. On failure returns -1 and sets
/// `*out_err` (if non-null).
///
/// # Safety
/// `handle` must be a valid pointer returned by `zb_uc_schema_from_table_json`
/// and not yet freed. `record_json` must be a NUL-terminated C string.
/// `out_bytes` and `out_len` must be non-null and writable.
#[no_mangle]
pub unsafe extern "C" fn zb_uc_encode_json(
    handle: *const ZbUcSchema,
    record_json: *const c_char,
    out_bytes: *mut *mut u8,
    out_len: *mut usize,
    out_err: *mut *mut c_char,
) -> c_int {
    if handle.is_null() || record_json.is_null() || out_bytes.is_null() || out_len.is_null() {
        set_err(out_err, "null argument".into());
        return -1;
    }
    let handle = &*handle;

    let json = match std::ffi::CStr::from_ptr(record_json).to_str() {
        Ok(s) => s,
        Err(e) => {
            set_err(out_err, format!("record JSON is not valid UTF-8: {e}"));
            return -1;
        }
    };

    let options = DeserializeOptions::new().deny_unknown_fields(false);
    let mut de = serde_json::Deserializer::from_str(json);
    let dynamic = match DynamicMessage::deserialize_with_options(handle.message.clone(), &mut de, &options) {
        Ok(m) => m,
        Err(e) => {
            set_err(out_err, format!("failed to map record to protobuf: {e}"));
            return -1;
        }
    };
    if let Err(e) = de.end() {
        set_err(out_err, format!("record JSON has trailing data: {e}"));
        return -1;
    }

    // into_boxed_slice() forces capacity == len so the C side can hand the exact
    // (ptr, len) back to zb_uc_free_bytes without tracking a separate capacity.
    let boxed = dynamic.encode_to_vec().into_boxed_slice();
    *out_len = boxed.len();
    *out_bytes = Box::into_raw(boxed) as *mut u8;
    0
}

/// Free a buffer returned by `zb_uc_encode_json`.
///
/// # Safety
/// `(ptr, len)` must be a pair previously produced by `zb_uc_encode_json` and
/// not yet freed. Passing a null `ptr` is a no-op.
#[no_mangle]
pub unsafe extern "C" fn zb_uc_free_bytes(ptr: *mut u8, len: usize) {
    if ptr.is_null() {
        return;
    }
    let slice = std::slice::from_raw_parts_mut(ptr, len);
    drop(Box::from_raw(slice as *mut [u8]));
}

/// Free a schema handle returned by `zb_uc_schema_from_table_json`.
///
/// # Safety
/// `handle` must be a pointer from `zb_uc_schema_from_table_json` not yet freed.
/// Passing null is a no-op. After this call the descriptor-bytes pointer handed
/// out at creation is dangling and must not be used.
#[no_mangle]
pub unsafe extern "C" fn zb_uc_schema_free(handle: *mut ZbUcSchema) {
    if handle.is_null() {
        return;
    }
    drop(Box::from_raw(handle));
}

/// Free an error string produced by any of the functions above.
///
/// # Safety
/// `err` must be a pointer set into an `out_err` slot by this library and not yet
/// freed. Passing null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn zb_uc_free_err(err: *mut c_char) {
    if err.is_null() {
        return;
    }
    drop(std::ffi::CString::from_raw(err));
}
