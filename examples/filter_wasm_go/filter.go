package main

import (
	"fmt"
	"io"
	"net/http"
	"unsafe"
)

var resultBuffer []byte

//export go_filter
func go_filter(tag *uint8, tag_len uint, time_sec uint, time_nsec uint, record *uint8, record_len uint) *uint8 {
	// 复制原始数据
	brecord := make([]byte, record_len)
	for i := uint(0); i < record_len; i++ {
		brecord[i] = *(*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(record)) + uintptr(i)))
	}

	// 调用外部 API
	fmt.Println("调用API...")
	resp, err := http.Get("https://httpbin.org/ip")
	if err != nil {
		fmt.Printf("API失败: %v\n", err)
		return &brecord[0]
	}
	defer resp.Body.Close()

	// 读取响应
	body, _ := io.ReadAll(resp.Body)
	fmt.Printf("API响应: %s\n", string(body))

	// 返回原始数据
	resultBuffer = make([]byte, len(brecord)+1)
	copy(resultBuffer, brecord)
	resultBuffer[len(brecord)] = 0

	return &resultBuffer[0]
}

func main() {}
