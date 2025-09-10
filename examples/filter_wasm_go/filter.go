package main

import (
	"encoding/json"
	"fmt"
	"time"
	"unsafe"
)

var resultBuffer []byte

//export go_filter
func go_filter(tag *uint8, tag_len uint, time_sec uint, time_nsec uint, record *uint8, record_len uint) *uint8 {
	// 参数验证
	if tag == nil || record == nil {
		return nil
	}

	// 长度限制
	if tag_len > 10240 || record_len > 1048576 { // 10KB tag, 1MB record 限制
		return nil
	}

	if tag_len == 0 || record_len == 0 {
		return nil
	}

	defer func() {
		if r := recover(); r != nil {
			fmt.Printf("Filter panic: %v\n", r)
			resultBuffer = nil
		}
	}()

	// 创建切片
	btag := make([]byte, tag_len)
	brecord := make([]byte, record_len)

	// 安全复制数据
	for i := uint(0); i < tag_len; i++ {
		btag[i] = *(*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(tag)) + uintptr(i)))
	}

	for i := uint(0); i < record_len; i++ {
		brecord[i] = *(*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(record)) + uintptr(i)))
	}

	now := time.Unix(int64(time_sec), int64(time_nsec))

	// 使用标准库 JSON 解析（更安全）
	var record_map map[string]interface{}
	if err := json.Unmarshal(brecord, &record_map); err != nil {
		fmt.Printf("JSON unmarshal error: %v\n", err)
		return nil
	}

	// 添加字段
	record_map["time"] = now.Format(time.RFC3339Nano)
	record_map["tag"] = string(btag)
	record_map["original"] = string(brecord)

	// 序列化回 JSON
	result, err := json.Marshal(record_map)
	if err != nil {
		fmt.Printf("JSON marshal error: %v\n", err)
		return nil
	}

	// 存储结果
	resultBuffer = append(result, 0) // 添加 null 终止符

	return &resultBuffer[0]
}

func main() {}
