// Import pure and fast JSON library written in Rust
use serde_json::json;
use serde_json::Value;
// Import chrono library to handle time related operation conveniently
use chrono::{Utc, TimeZone};
use std::slice;
use std::str;
use std::os::raw::c_char;
use std::io::Write;

#[no_mangle]
pub extern "C" fn rust_filter(tag: *const c_char, tag_len: u32, time_sec: u32, time_nsec: u32, record: *const c_char, record_len: u32) -> *const u8 {
    let slice_tag: &[u8] = unsafe { slice::from_raw_parts(tag as *const u8, tag_len as usize) };
    let slice_record: &[u8] = unsafe { slice::from_raw_parts(record as *const u8, record_len as usize) };

    let mut vt: Vec<u8> = Vec::new();
    vt.write(slice_tag).expect("Unable to write");
    let vtag = str::from_utf8(&vt).unwrap();

    // 输出关键信息
    println!("Tag: {}, Size: {}", vtag, record_len);

    let v: Value = serde_json::from_slice(slice_record).unwrap();
    let dt = Utc.timestamp(time_sec as i64, time_nsec);
    let time = dt.format("%Y-%m-%dT%H:%M:%S.%9f %z").to_string();

    // 输出 Kubernetes 关键信息
    if let Some(k8s) = v.get("kubernetes") {
        if let Some(pod) = k8s.get("pod_name") {
            print!("Pod: {}", pod.as_str().unwrap_or("N/A"));
        }
        if let Some(ns) = k8s.get("namespace_name") {
            print!(", NS: {}", ns.as_str().unwrap_or("N/A"));
        }
        if let Some(container) = k8s.get("container_name") {
            println!(", Container: {}", container.as_str().unwrap_or("N/A"));
        }
    }

    let message = json!({
        "message": v["message"],
        "time": format!("{}", time),
        "tag": vtag,
        "original": v.to_string(),
        "lang": "Rust",
    });

    let buf: String = message.to_string();
    buf.as_ptr()
}
