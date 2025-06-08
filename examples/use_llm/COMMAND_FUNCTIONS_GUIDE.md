# ESP32-S3 语音命令函数封装使用指南

## 概述

本文档介绍了ESP32-S3语音识别项目中封装的命令词管理函数，这些函数让添加和管理自定义语音命令变得更加简单和模块化。

## 核心功能

### 1. 命令词配置结构体

```c
typedef struct {
    int command_id;          // 命令ID
    const char* pinyin;      // 命令的拼音表示
    const char* description; // 命令的中文描述
} command_config_t;
```

### 2. 自定义命令词列表

```c
static const command_config_t custom_commands[] = {
    {COMMAND_TURN_ON_LIGHT, "bang wo kai deng", "帮我开灯"},
    {COMMAND_TURN_OFF_LIGHT, "bang wo guan deng", "帮我关灯"},
    {COMMAND_BYE_BYE, "bai bai", "拜拜"},
    {COMMAND_SAFETY_HOUSE_STATUS, "xian zai an quan wu qing kuang ru he", "现在安全屋情况如何"}
};
```

## 主要函数

### 1. configure_custom_commands()

**功能**: 配置所有自定义命令词

**原型**:
```c
static esp_err_t configure_custom_commands(esp_mn_iface_t *multinet, model_iface_data_t *mn_model_data)
```

**参数**:
- `multinet`: 命令词识别接口指针
- `mn_model_data`: 命令词模型数据指针

**返回值**:
- `ESP_OK`: 配置成功
- `ESP_FAIL`: 配置失败

**功能说明**:
- 清除现有命令词
- 批量添加自定义命令词列表中的所有命令
- 更新命令词到模型
- 提供详细的日志输出
- 显示配置结果统计

### 2. add_single_command()

**功能**: 动态添加单个命令词

**原型**:
```c
static esp_err_t add_single_command(int command_id, const char* pinyin, const char* description)
```

**参数**:
- `command_id`: 命令ID
- `pinyin`: 命令的拼音表示
- `description`: 命令的中文描述（用于日志）

**返回值**:
- `ESP_OK`: 添加成功
- `ESP_FAIL`: 添加失败

**使用场景**: 在运行时动态添加新的命令词

### 3. get_command_description()

**功能**: 获取命令词的中文描述

**原型**:
```c
static const char* get_command_description(int command_id)
```

**参数**:
- `command_id`: 命令ID

**返回值**: 命令的中文描述，如果未找到返回"未知命令"

## 使用方法

### 添加新的命令词

1. **定义命令ID**:
```c
#define COMMAND_NEW_FEATURE 316  // "新功能"
```

2. **添加到命令列表**:
```c
static const command_config_t custom_commands[] = {
    // ... 现有命令 ...
    {COMMAND_NEW_FEATURE, "xin gong neng", "新功能"}
};
```

3. **在命令处理逻辑中添加处理**:
```c
else if (command_id == COMMAND_NEW_FEATURE)
{
    ESP_LOGI(TAG, "💡 执行新功能命令");
    // 添加你的功能代码
}
```

### 运行时动态添加命令

```c
// 在系统初始化完成后调用
esp_err_t ret = add_single_command(316, "xin gong neng", "新功能");
if (ret == ESP_OK) {
    ESP_LOGI(TAG, "新命令添加成功");
}
```

## 日志输出示例

### 配置过程日志
```
I (1797) 语音识别: 开始配置自定义命令词...
I (1797) 语音识别: 添加命令词 [309]: 帮我开灯 (bang wo kai deng)
I (1797) 语音识别: ✓ 命令词 [309] 添加成功
I (1797) 语音识别: 添加命令词 [308]: 帮我关灯 (bang wo guan deng)
I (1797) 语音识别: ✓ 命令词 [308] 添加成功
I (1797) 语音识别: 命令词配置完成: 成功 4 个, 失败 0 个
```

### 识别结果日志
```
I (329035) 语音识别: 🎯 检测到命令词: ID=309, 置信度=0.61, 内容= bang wo kai deng, 命令='帮我开灯'
```

## 优势

1. **模块化设计**: 命令词管理与业务逻辑分离
2. **易于扩展**: 只需修改配置数组即可添加新命令
3. **详细日志**: 提供完整的配置和识别过程日志
4. **错误处理**: 完善的错误检查和处理机制
5. **动态管理**: 支持运行时添加命令词
6. **统一接口**: 提供一致的API接口

## 注意事项

1. **拼音格式**: 确保拼音格式正确，使用空格分隔音节
2. **命令ID**: 确保命令ID唯一，避免冲突
3. **内存管理**: 系统会自动管理命令词相关内存
4. **模型限制**: 受MultiNet7模型支持的命令词数量限制（最大400个）

## 故障排除

### 命令词不被识别
1. 检查拼音格式是否正确
2. 确认命令ID是否唯一
3. 查看配置日志确认命令是否成功添加
4. 检查模型是否支持该拼音组合

### 配置失败
1. 检查内存是否充足
2. 确认模型是否正确初始化
3. 查看错误日志获取详细信息

## 示例代码

完整的使用示例请参考 `main/main.cc` 文件中的实现。
