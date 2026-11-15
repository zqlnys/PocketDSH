# 系统能力边界：提醒、电话、消息

PocketDSH 不会使用描述文件、MDM、私有 entitlement 或漏洞去绕过 iOS 沙盒。配置描述文件通常只能做设备管理/限制，**不能给第三方 App 开放静默拨号、读取短信或自动发送消息的能力**。

## 已实现

### 创建提醒

工具 `schedule_reminder`：

- 使用 EventKit `requestFullAccessToReminders()` 请求权限；
- 在用户默认提醒列表中创建真实 `EKReminder`；
- 返回提醒创建结果。

这是 **提醒事项**，不是系统“时钟”App 的闹钟。若需要 iOS 26+ 的 AlarmKit，需要单独适配和权限验证。

### 准备消息

工具 `prepare_message`：

- 不自动发送；
- 通过 `MFMessageComposeViewController` 预填收件人和正文；
- 用户必须点击系统编辑器里的发送按钮。

不能读取 Messages 数据库、自动回复或静默发送。

### 准备电话

工具 `prepare_call`：

- 不自动拨号；
- 打开 `tel:`，由系统弹出拨号确认。

## 当前未实现

- Clock App 的系统闹钟；
- AlarmKit（iOS 26+）；
- 读取/自动回复 Messages；
- 后台发送短信/iMessage；
- 通话记录读取。

## 结论

这些能力不是“没申请权限”，而是 iOS 的公开 API 边界。描述文件或漏洞不是可接受的产品实现方式。当前实现只提供“准备动作 + 用户确认”，并明确标记未在真实设备上验证。
