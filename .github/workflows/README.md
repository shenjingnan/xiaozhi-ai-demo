# GitHub Actions 工作流说明

## build-examples.yml

这个工作流用于自动构建和验证 `examples/` 目录下的所有 ESP-IDF 示例项目。

### 触发条件

- 当有 Pull Request 提交到 `main` 分支时
- 当 `examples/` 目录或工作流文件有更改时
- 手动触发（通过 GitHub Actions 界面）

### 构建矩阵

工作流会在以下平台上进行构建测试：
- Ubuntu (ubuntu-latest)
- Windows (windows-latest)
- macOS (macos-latest)

### 排除项目

- `examples/control-led/` - 这是一个 PlatformIO 项目，不使用 ESP-IDF 构建系统

### 构建环境

使用官方 ESP-IDF Docker 镜像：`espressif/idf:release-v5.4`

### 输出

- **构建成功**：生成构建产物（.bin 和 .elf 文件）并上传为 artifacts
- **构建失败**：上传构建日志以便调试
- **构建摘要**：在 GitHub Actions 的 Summary 中显示每个平台的构建结果

### 本地测试

可以使用以下命令在本地测试构建：

```bash
# Linux/macOS
docker run --rm -v $PWD:/project -w /project -u $(id -u):$(id -g) -e HOME=/tmp espressif/idf:release-v5.4 idf.py build

# Windows
docker run --rm -v ${PWD}:/project -w /project espressif/idf:release-v5.4 idf.py build
```