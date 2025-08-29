package main

import (
	"fmt"
	"path/filepath"
	"regexp"
	"strings"
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
	fmt.Printf("原始 JSON 记录: %s\n", br)

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

	var arena fastjson.Arena
	obj.Set("time", arena.NewString(now.String()))
	obj.Set("tag", arena.NewString(string(btag)))
	obj.Set("original", arena.NewString(br))

	fmt.Println("开始路径信息提取...")
	// 新增：提取路径信息
	pathInfo := extractPathInfo(obj, &arena)
	if pathInfo != nil {
		obj.Set("path_info", pathInfo)
		fmt.Println("✅ 路径信息提取成功")
	} else {
		fmt.Println("⚠️  未找到匹配的路径信息")
	}

	s := obj.String() + string(rune(0))
	rv := []byte(s)

	fmt.Printf("最终输出长度: %d\n", len(rv))
	fmt.Println("=== WASM Filter 处理完成 ===\n")

	return &rv[0]
}

// 提取路径信息的函数
func extractPathInfo(obj *fastjson.Object, arena *fastjson.Arena) *fastjson.Value {
	fmt.Println("🔍 开始查找路径字段...")

	// 尝试从不同字段获取路径信息
	pathFields := []string{"path", "file", "source", "filename", "_path"}

	var logPath string
	var foundField string

	for _, field := range pathFields {
		fmt.Printf("检查字段: %s\n", field)
		if pathValue := obj.Get(field); pathValue != nil {
			if pathBytes := pathValue.GetStringBytes(); pathBytes != nil {
				logPath = string(pathBytes)
				foundField = field
				fmt.Printf("✅ 在字段 '%s' 中找到路径: %s\n", field, logPath)
				break
			}
		}
	}

	if logPath == "" {
		fmt.Println("❌ 未在任何字段中找到路径信息")
		// 输出所有可用字段供调试
		fmt.Println("当前记录中的所有字段:")
		obj.Visit(func(key []byte, v *fastjson.Value) {
			fmt.Printf("  - %s: %s\n", string(key), v.String())
		})
		return nil
	}

	fmt.Printf("🎯 开始匹配路径模式: %s\n", logPath)

	// 匹配路径模式并提取通配符值
	wildcards := matchKubeletPath(logPath)
	if len(wildcards) == 0 {
		fmt.Printf("❌ 路径不匹配 kubelet 模式: %s\n", logPath)
		return nil
	}

	fmt.Printf("🎉 路径匹配成功！提取到的值:\n")
	fmt.Printf("  Pod UID: %s\n", wildcards[0])
	fmt.Printf("  Volume Name: %s\n", wildcards[1])
	fmt.Printf("  Log Filename: %s\n", wildcards[2])

	// 创建路径信息对象
	pathInfoObj := arena.NewObject()
	pathInfoObj.Set("matched_path", arena.NewString(logPath))
	pathInfoObj.Set("found_in_field", arena.NewString(foundField))
	pathInfoObj.Set("pod_uid", arena.NewString(wildcards[0]))
	pathInfoObj.Set("volume_name", arena.NewString(wildcards[1]))
	pathInfoObj.Set("log_filename", arena.NewString(wildcards[2]))

	return pathInfoObj
}

// 匹配 kubelet 路径模式的函数
func matchKubeletPath(path string) []string {
	fmt.Printf("🔍 使用正则表达式匹配路径...\n")

	// 方法1: 使用正则表达式匹配
	pattern := `^/var/lib/kubelet/pods/([^/]+)/volumes/kubernetes\.io~csi/([^/]+)/mount/([^/]+\.log)$`
	re := regexp.MustCompile(pattern)
	matches := re.FindStringSubmatch(path)

	if len(matches) == 4 {
		fmt.Println("✅ 正则表达式匹配成功")
		// matches[0] 是完整匹配，matches[1-3] 是通配符值
		return matches[1:]
	}

	fmt.Println("❌ 正则表达式匹配失败，尝试 filepath 匹配...")

	// 方法2: 使用 filepath.Match 进行模式匹配（备用方案）
	return matchWithFilepath(path)
}

// 使用 filepath 进行路径匹配的备用方法
func matchWithFilepath(path string) []string {
	pattern := "/var/lib/kubelet/pods/*/volumes/kubernetes.io~csi/*/mount/*.log"

	fmt.Printf("使用 filepath 模式匹配: %s\n", pattern)

	matched, err := filepath.Match(pattern, path)
	if err != nil {
		fmt.Printf("❌ filepath.Match 错误: %v\n", err)
		return nil
	}

	if !matched {
		fmt.Println("❌ filepath.Match 匹配失败")
		return nil
	}

	fmt.Println("✅ filepath.Match 匹配成功，开始解析路径...")

	// 手动解析路径提取通配符值
	parts := strings.Split(path, "/")
	fmt.Printf("路径分段数量: %d\n", len(parts))

	for i, part := range parts {
		fmt.Printf("  [%d]: %s\n", i, part)
	}

	if len(parts) < 11 {
		fmt.Printf("❌ 路径分段数量不足，期望至少11个，实际%d个\n", len(parts))
		return nil
	}

	// 验证路径结构
	if parts[1] != "var" || parts[2] != "lib" || parts[3] != "kubelet" ||
		parts[4] != "pods" || parts[6] != "volumes" ||
		parts[7] != "kubernetes.io~csi" || parts[9] != "mount" {
		fmt.Println("❌ 路径结构验证失败")
		return nil
	}

	podUID := parts[5]       // 第一个 *
	volumeName := parts[8]   // 第二个 *
	logFilename := parts[10] // 第三个 * (*.log)

	// 验证日志文件扩展名
	if !strings.HasSuffix(logFilename, ".log") {
		fmt.Printf("❌ 文件名不以 .log 结尾: %s\n", logFilename)
		return nil
	}

	fmt.Println("✅ 路径结构验证成功")
	return []string{podUID, volumeName, logFilename}
}

func main() {}
