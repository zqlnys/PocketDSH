# 系统能力边界：闹钟、电话、消息

PocketDSH 不会使用描述文件、MDM、私有 entitlement 或漏洞去绕过 iOS 沙盒。配置描述文件通常只能做设备管理/限制，**不能给第三方 App 开放静默拨号、读取短信或自动发送消息的能力**。

## 闹钟 / 提醒

可选合法路径：

- **Local Notifications**：App 可请求通知权限，在指定时间提醒用户；不是系统“时钟”闹钟。
- **EventKit Reminders**：请求提醒事项权限后创建带时间的提醒。
- **AlarmKit（iOS 26+）**：Apple 提供的闹钟 API，可创建系统闹钟体验；需要目标 SDK 和用户授权，不能假设 iOS 17 设备可用。
- **Clock 闹钟**：第三方 App 不能静默写入系统“时钟”App 的闹钟列表。

当前 PocketDSH 尚未实现这些工具。实现时应由 Agent 生成动作，用户在 App 内确认后调用公开 API。

## 电话

- 合法方式：打开 `tel:` URL，系统弹出拨号/确认界面。
- 不能：静默拨号、后台拨号、读取通话记录。
- 这不是“权限没申请”，而是 iOS 的公开 API 边界。

## 消息

- 合法方式：`MFMessageComposeViewController` 预填收件人和正文，用户点击发送；或打开 `sms:` URL。
- 不能：读取 Messages 数据库、自动回复、静默发送短信/iMessage。
- 不存在可以通过普通描述文件或漏洞合法获得的“自动回消息”能力；PocketDSH 不会尝试这种做法。

## Agent 应该如何表达

对于这些动作，工具结果应明确为：

```text
prepared: "打开电话拨号，号码 +1..."
requires_user_confirmation: true
```

而不是伪造 `completed: true`。

## 结论

可以后续增加“准备动作 + 用户确认 + 调用公开系统 UI”的集成，但不能把其宣传为静默自动化。当前版本没有实现闹钟、电话、消息工具；这属于未验证/未实现能力。
