# 外网 SSH 访问：端口与排查过程

## 当前状态

**sshd 监听两个端口**（由 `ssh.socket` 管理，不是 `sshd_config` 的 `Port`）：

```
0.0.0.0:22     0.0.0.0:8443
[::]:22        [::]:8443
```

| 端口 | 用途 |
| --- | --- |
| 22 | WiFi / 普通网络 |
| **8443** | **移动流量**（运营商常封出站 22） |

## 三个必须同时成立的环节

**这一路排查的教训是：SSH 连不上有四个独立环节，任何一个断了表现都不同。**

| 环节 | 检查命令 | 断了的表现 |
| --- | --- | --- |
| 1. socket 监听 | `ss -lntp 'sport = :8443'` | Connection refused |
| 2. **防火墙规则真的在内核里** | `nft list chain ip filter ufw-user-input \| grep 8443` | Connection refused |
| 3. 上游/云防火墙 | 外部检测（见下） | Timeout 或 refused |
| 4. 算法策略 | `sshd -T \| grep pubkeyaccepted` | 握手后 Permission denied |

### 第 2 条踩过的坑

**`ufw status` 显示 8443 ALLOW，但 nftables 里没有这条规则。**

```
$ ufw status | grep 8443
8443/tcp   ALLOW   Anywhere          ← 说的是「已放行」

$ nft list chain ip filter ufw-user-input | grep 8443
(无输出)                              ← 内核里根本没有

$ ufw reload                          ← 修法
$ nft list chain ip filter ufw-user-input | grep 8443
tcp dport 8443 counter packets 0 bytes 0 accept
```

**`ufw allow` 之后必须 `ufw reload`，否则规则只写在 ufw 自己的状态里。**

### 第 4 条：OpenSSH 9.6 默认拒绝 RSA-SHA1

```
userauth_pubkey: signature algorithm ssh-rsa not in PubkeyAcceptedAlgorithms
```

**已加回**（`/etc/ssh/sshd_config` 末尾）：

```
PubkeyAcceptedAlgorithms +ssh-rsa
```

### 加端口的正确方式

**Ubuntu 22.10+ 用 `ssh.socket` 激活，改 `sshd_config` 的 `Port` 不会新增监听。**

**要加 drop-in：**

```ini
# /etc/systemd/system/ssh.socket.d/8443.conf
[Socket]
ListenStream=0.0.0.0:8443
ListenStream=[::]:8443
```

```bash
systemctl daemon-reload
systemctl restart ssh.socket      # reload 不够，必须 restart
```

## 外部可达性怎么验证

**服务器自己测不了自己的外网端口，用外部节点：**

```bash
# 发起
curl -s -H "Accept: application/json" \
  "https://check-host.net/check-tcp?host=<IP>:<PORT>&max_nodes=3"
# 返回一个 request_id，等十几秒后取结果
curl -s -H "Accept: application/json" \
  "https://check-host.net/check-result/<request_id>"
```

**返回里有 `{"address":"...","time":0.09}` 就是通的；`null` 或 error 是不通。**

**2026-11-21 实测：22 和 8443 从西班牙、罗马尼亚、迈阿密三个节点都能连上。**

## 四层的判定表

| 现象 | 说明 |
| --- | --- |
| `Connection refused` 且内核有 accept 规则 | 上游拦的 |
| `Connection refused` 且内核没规则 | `ufw allow` 没 reload |
| `Timeout` | 上游 DROP（不回 RST） |
| 握手成功但 `Permission denied (publickey,password)` | **服务器没问题**，是密钥/密码不对 |
| `signature algorithm ssh-rsa not in ...` | 算法策略，加 `+ssh-rsa` |

**最后一行是这次的关键：`Permission denied (publickey,password)` 出现在认证阶段，
说明 TCP 连接和 SSH 握手都已经成功——它不代表连不上。**
