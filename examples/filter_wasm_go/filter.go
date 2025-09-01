// package main
//
// import (
// 	"fmt"
// 	"time"
// 	"unsafe"
//
// 	"github.com/valyala/fastjson"
// )
//
// //export go_filter
// func go_filter(tag *uint8, tag_len uint, time_sec uint, time_nsec uint, record *uint8, record_len uint) *uint8 {
// 	// 添加 defer 来捕获 panic
// 	defer func() {
// 		if r := recover(); r != nil {
// 			fmt.Printf("❌ WASM Filter Panic: %v\n", r)
// 		}
// 	}()
//
// 	fmt.Println("=== WASM Filter 开始处理 ===")
//
// 	// 安全检查输入参数
// 	if tag == nil || record == nil || tag_len == 0 || record_len == 0 {
// 		fmt.Println("❌ 输入参数无效")
// 		return nil
// 	}
//
// 	btag := unsafe.Slice(tag, tag_len)
// 	brecord := unsafe.Slice(record, record_len)
// 	now := time.Unix(int64(time_sec), int64(time_nsec))
//
// 	fmt.Printf("接收到的标签: %s\n", string(btag))
// 	fmt.Printf("接收到的记录长度: %d\n", record_len)
// 	fmt.Printf("时间戳: %s\n", now.String())
//
// 	br := string(brecord)
//
// 	var p fastjson.Parser
// 	value, err := p.Parse(br)
// 	if err != nil {
// 		fmt.Printf("❌ JSON 解析失败: %v\n", err)
// 		return nil
// 	}
// 	fmt.Println("✅ JSON 解析成功")
//
// 	obj, err := value.Object()
// 	if err != nil {
// 		fmt.Printf("❌ 获取 JSON 对象失败: %v\n", err)
// 		return nil
// 	}
//
// 	// 输出所有labels
// 	extractAndPrintLabels(obj)
//
// 	var arena fastjson.Arena
// 	obj.Set("time", arena.NewString(now.String()))
// 	obj.Set("tag", arena.NewString(string(btag)))
// 	obj.Set("original", arena.NewString(br))
//
// 	// 简化的内存管理
// 	result := obj.String()
//
// 	// 使用全局变量存储结果，避免被GC回收
// 	globalResult = make([]byte, len(result)+1)
// 	copy(globalResult, []byte(result))
// 	globalResult[len(result)] = 0 // null terminator
//
// 	fmt.Printf("最终输出长度: %d\n", len(globalResult)-1)
// 	fmt.Println("=== WASM Filter 处理完成 ===\n")
//
// 	return &globalResult[0]
// }
//
// // 全局变量存储结果，防止被GC回收
// var globalResult []byte
//
// // 提取并打印所有labels
// func extractAndPrintLabels(obj *fastjson.Object) {
// 	defer func() {
// 		if r := recover(); r != nil {
// 			fmt.Printf("❌ 提取标签时发生错误: %v\n", r)
// 		}
// 	}()
//
// 	fmt.Println("🏷️  开始提取标签信息...")
//
// 	// 检查kubernetes字段
// 	kubernetesValue := obj.Get("kubernetes")
// 	if kubernetesValue == nil {
// 		fmt.Println("❌ 未找到kubernetes字段")
// 		return
// 	}
//
// 	kubernetesObj, err := kubernetesValue.Object()
// 	if err != nil {
// 		fmt.Println("❌ kubernetes字段不是对象")
// 		return
// 	}
//
// 	// 提取labels
// 	labelsValue := kubernetesObj.Get("labels")
// 	if labelsValue == nil {
// 		fmt.Println("❌ 未找到labels字段")
// 		return
// 	}
//
// 	labelsObj, err := labelsValue.Object()
// 	if err != nil {
// 		fmt.Println("❌ labels字段不是对象")
// 		return
// 	}
//
// 	fmt.Println("📋 原始记录中的所有标签:")
// 	fmt.Println("==========================================")
//
// 	// 遍历所有labels
// 	labelsObj.Visit(func(key []byte, v *fastjson.Value) {
// 		if len(key) == 0 || v == nil {
// 			return
// 		}
//
// 		labelKey := string(key)
// 		labelValue := ""
//
// 		switch v.Type() {
// 		case fastjson.TypeString:
// 			labelValue = string(v.GetStringBytes())
// 		case fastjson.TypeNumber:
// 			labelValue = v.String()
// 		case fastjson.TypeTrue:
// 			labelValue = "true"
// 		case fastjson.TypeFalse:
// 			labelValue = "false"
// 		case fastjson.TypeNull:
// 			labelValue = "null"
// 		default:
// 			labelValue = v.String()
// 		}
//
// 		fmt.Printf("🏷️  %s: %s\n", labelKey, labelValue)
// 	})
//
// 	fmt.Println("==========================================")
// 	fmt.Println("✅ 标签信息提取完成")
//
// 	// 额外输出一些基本的kubernetes信息
// 	printKubernetesInfo(kubernetesObj)
// }
//
// func printKubernetesInfo(kubernetesObj *fastjson.Object) {
// 	fmt.Println("\n📦 其他Kubernetes信息:")
// 	fmt.Println("------------------------------------------")
//
// 	fields := map[string]string{
// 		"pod_name":       "Pod名称",
// 		"namespace_name": "命名空间",
// 		"container_name": "容器名称",
// 		"host":           "主机名",
// 	}
//
// 	for field, label := range fields {
// 		if value := kubernetesObj.Get(field); value != nil && value.Type() == fastjson.TypeString {
// 			fmt.Printf("%s: %s\n", label, string(value.GetStringBytes()))
// 		}
// 	}
//
// 	fmt.Println("------------------------------------------")
// }
//
// func main() {}

package main

import (
	"fmt"
)

//export go_filter
func go_filter(tag *uint8, tag_len uint, time_sec uint, time_nsec uint, record *uint8, record_len uint) *uint8 {
	fmt.Println("WASM filter called successfully!")

	// 直接返回原始记录，不做任何处理
	if record == nil || record_len == 0 {
		return nil
	}

	return record
}

func main() {}
