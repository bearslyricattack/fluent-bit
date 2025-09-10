package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
	"unsafe"
)

var resultBuffer []byte

//export go_filter
func go_filter(tag *uint8, tag_len uint, time_sec uint, time_nsec uint, record *uint8, record_len uint) *uint8 {
	if tag == nil || record == nil || tag_len == 0 || record_len == 0 {
		return nil
	}

	// 复制数据
	btag := make([]byte, tag_len)
	brecord := make([]byte, record_len)

	for i := uint(0); i < tag_len; i++ {
		btag[i] = *(*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(tag)) + uintptr(i)))
	}

	for i := uint(0); i < record_len; i++ {
		brecord[i] = *(*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(record)) + uintptr(i)))
	}

	// 解析原始记录
	var record_map map[string]interface{}
	json.Unmarshal(brecord, &record_map)

	// 调用外部 API
	fmt.Println("开始调用外部API: https://httpbin.org/ip")

	resp, err := http.Get("https://httpbin.org/ip")
	if err == nil {
		fmt.Printf("API调用成功，状态码: %d\n", resp.StatusCode)
		defer resp.Body.Close()

		body, readErr := io.ReadAll(resp.Body)
		if readErr != nil {
			fmt.Printf("读取响应体失败: %v\n", readErr)
		} else {
			fmt.Printf("API响应内容: %s\n", string(body))
		}

		var apiData map[string]interface{}
		unmarshalErr := json.Unmarshal(body, &apiData)
		if unmarshalErr != nil {
			fmt.Printf("API响应JSON解析失败: %v\n", unmarshalErr)
		} else {
			fmt.Printf("API响应解析成功，数据: %+v\n", apiData)
		}
		// 添加 API 数据到记录
		record_map["external_data"] = apiData
	} else {
		record_map["external_data"] = map[string]interface{}{"error": err.Error()}
	}

	// 添加时间戳
	record_map["processed_at"] = time.Now().Format(time.RFC3339)
	record_map["tag"] = string(btag)

	// 序列化结果
	result, _ := json.Marshal(record_map)

	resultBuffer = make([]byte, len(result)+1)
	copy(resultBuffer, result)
	resultBuffer[len(result)] = 0

	return &resultBuffer[0]
}

func main() {}
