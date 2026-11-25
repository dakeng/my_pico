# MyPico 热点同步到 iCloud Obsidian（iPhone 快捷指令指南）

通过连接 iPhone 个人热点，MyPico 会以 STA 模式连入热点并广播 `mypico.local`，无需切换手机 Wi-Fi（手机保持蜂窝网络在线，iCloud 立即同步）。

---

## 一、首次准备（只需做一次）

1. **在 iPhone 上开启 2.4GHz 热点**：
   打开 `设置 -> 个人热点`，开启 **「允许其他人加入」** 和 **「最大兼容性」**（ESP32-S3 仅支持 2.4 GHz Wi-Fi）。
2. **在 MyPico 上保存你的 iPhone 热点**：
   进入首页 `设置 -> Wi-Fi / 热点配网`（或 `传书 Transfer`），在停服状态下扫描并选择你的 iPhone 热点，输入热点密码保存一次即可。以后每次同步会自动复用这份凭据。

---

## 二、在 iPhone 上创建「同步 MyPico 笔记」快捷指令

打开 iPhone 自带的 **「快捷指令 (Shortcuts)」** App，新建一条快捷指令，依次添加以下 **5 个简单操作**：

1. **获取当前日期**：添加「日期」（默认「当前日期」）。
2. **格式化日期（用于自动校准设备时间）**：
   - 添加「格式化日期」
   - 日期格式选择 **「自定义」**，格式字符串填：`X`（Unix 秒级时间戳；如果不想校时也可以跳过第 1–2 步，直接请求 `http://mypico.local/api/sync`）。
3. **获取 URL 内容**：
   - URL 填：`http://mypico.local/api/sync?ts=[格式化后的日期]`
   - 方法：`GET`
4. **获取词典值**（添加两次）：
   - 第一次：键填 `filename`，输入为上一步的「URL 内容」→ 得到笔记文件名（如 `页边和听写.md`）
   - 第二次：键填 `markdown`，输入为「URL 内容」→ 得到完整 Markdown 文本
5. **存储文件**：
   - 文件内容选择 `markdown`（注意：先用「设定名称」把该文本命名为 `filename`，或直接把「存储文件」的子路径设为 `<你的笔记目录>/[filename]`）
   - 关闭「询问存储位置」，目标文件夹选到 `iCloud Drive / Obsidian / <你的库名>`
   - 子路径填：`阅读笔记/[filename]`（按你的实际目录调整）
   - **开启「如果文件存在则覆盖」**

> 建议把这条快捷指令「添加到主屏幕」，以后只需一键点击！

---

## 三、日常使用流程

1. 打开 iPhone 个人热点。
2. 在 MyPico 书架点击对应书籍的 **「同步」** 按钮，等待屏幕显示 **「已就绪，请点击 iPhone 快捷指令」**。
3. 在 iPhone 桌面点击 **「同步 MyPico 笔记」** 快捷指令图标：
   - 快捷指令会自动拉取该书最新阅读进度、阅读时长、最近阅读日期及全部划线，覆盖写入 iCloud Obsidian 笔记库。
   - 同时 MyPico 会自动把设备硬件 RTC 校准到手机时间，屏幕变为 **「同步完成！」**。
4. 点击 MyPico 上的 **「完成返回」** 关闭 Wi-Fi 省电。

---

## 四、导出的 Obsidian Markdown 示例

```markdown
---
分类:
  - "[[阅读笔记]]"
书名: 页边和听写
created: 2026-11-25
tags:
---

## 阅读记录

- 作者：埃琳娜·费兰特
- 阅读进度：30%
- 阅读时长：1h20min
- 最近阅读：2026-11-25
- 划线数：12

## 划线

> 那些字母临时组合在一起，肯定很不精确，而它们出现在我眼前，很接近我脑子里最初涌动的东西，但一经导出，就脱离我而存在。
```

---

## 五、接口参考（调试备用）

如果某些 iOS 版本下热点内 `mypico.local` 解析缓慢，也可以用屏幕显示的备用 IP（如 `http://172.20.10.2/api/sync`）直接测试：

- `GET /api/sync`（可选 `?ts=<unix_sec>`）：返回 JSON（含 `title`, `author`, `filename`, `progress`, `duration_sec`, `duration`, `last_read`, `highlight_count`, `markdown`）。
- `GET /api/sync/markdown`（可选 `?ts=<unix_sec>`）：直接返回纯 `text/markdown; charset=utf-8`，响应头 `X-MyPico-Filename` 带文件名。
- `POST /api/sync/ack`：可选回调 `{"ts": 1795610000}` 校准时间。
