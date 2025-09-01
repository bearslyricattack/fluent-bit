package main

import (
	"fmt"
	"time"
	"unsafe"

	"github.com/valyala/fastjson"
)

//export go_filter
func go_filter(tag *uint8, tag_len uint, time_sec uint, time_nsec uint, record *uint8, record_len uint) *uint8 {
	fmt.Println("=== WASM Filter 开始处理 ===")

	btag := unsafe.Slice(tag, tag_len)
	brecord := unsafe.Slice(record, record_len)
	now := time.Unix(int64(time_sec), int64(time_nsec))

	fmt.Printf("接收到的标签: %s\n", string(btag))
	fmt.Printf("接收到的记录长度: %d\n", record_len)
	fmt.Printf("时间戳: %s\n", now.String())

	br := string(brecord)

	var p fastjson.Parser
	value, err := p.Parse(br)
	if err != nil {
		fmt.Printf("❌ JSON 解析失败: %v\n", err)
		return nil
	}
	fmt.Println("✅ JSON 解析成功")

	obj, err := value.Object()
	if err != nil {
		fmt.Printf("❌ 获取 JSON 对象失败: %v\n", err)
		return nil
	}

	// 输出所有labels
	extractAndPrintLabels(obj)

	var arena fastjson.Arena
	obj.Set("time", arena.NewString(now.String()))
	obj.Set("tag", arena.NewString(string(btag)))
	obj.Set("original", arena.NewString(br))

	s := obj.String() + string(rune(0))
	rv := []byte(s)

	fmt.Printf("最终输出长度: %d\n", len(rv))
	fmt.Println("=== WASM Filter 处理完成 ===\n")

	return &rv[0]
}

// 提取并打印所有labels
func extractAndPrintLabels(obj *fastjson.Object) {
	fmt.Println("🏷️  开始提取标签信息...")

	// 检查kubernetes字段
	kubernetesValue := obj.Get("kubernetes")
	if kubernetesValue == nil {
		fmt.Println("❌ 未找到kubernetes字段")
		return
	}

	kubernetesObj, err := kubernetesValue.Object()
	if err != nil {
		fmt.Println("❌ kubernetes字段不是对象")
		return
	}

	// 提取labels
	labelsValue := kubernetesObj.Get("labels")
	if labelsValue == nil {
		fmt.Println("❌ 未找到labels字段")
		return
	}

	labelsObj, err := labelsValue.Object()
	if err != nil {
		fmt.Println("❌ labels字段不是对象")
		return
	}

	fmt.Println("📋 原始记录中的所有标签:")
	fmt.Println("==========================================")

	// 遍历所有labels
	labelsObj.Visit(func(key []byte, v *fastjson.Value) {
		labelKey := string(key)
		labelValue := ""

		if v.Type() == fastjson.TypeString {
			labelValue = string(v.GetStringBytes())
		} else {
			labelValue = v.String()
		}

		fmt.Printf("🏷️  %s: %s\n", labelKey, labelValue)
	})

	fmt.Println("==========================================")
	fmt.Println("✅ 标签信息提取完成")

	// 额外输出一些基本的kubernetes信息
	fmt.Println("\n📦 其他Kubernetes信息:")
	fmt.Println("------------------------------------------")

	if podName := kubernetesObj.Get("pod_name"); podName != nil {
		fmt.Printf("Pod名称: %s\n", string(podName.GetStringBytes()))
	}

	if namespace := kubernetesObj.Get("namespace_name"); namespace != nil {
		fmt.Printf("命名空间: %s\n", string(namespace.GetStringBytes()))
	}

	if containerName := kubernetesObj.Get("container_name"); containerName != nil {
		fmt.Printf("容器名称: %s\n", string(containerName.GetStringBytes()))
	}

	if host := kubernetesObj.Get("host"); host != nil {
		fmt.Printf("主机名: %s\n", string(host.GetStringBytes()))
	}

	fmt.Println("------------------------------------------")
}

func main() {}
