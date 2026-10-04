# RK-KVM

在 **创龙 TL3506-MiniEVM-NAND（瑞芯微 RK3506，armv7l）** 上实现的 **用户态 IP-KVM**：
通过 USB 采集棒采集被控机 HDMI，用板子的 USB OTG 口模拟键鼠（HID），浏览器远程控制；
并提供带登录认证的网页界面与可执行 shell 的维护后台。

> 由于该内核**没有 `uvcvideo` 驱动、也没有 `usb_f_hid`**，本项目**完全在用户态**实现视频采集与 HID，
> 无需重编内核（板子无 BSP/SDK 的情况下尤其有用）。

## 功能

- **视频**：用户态 UVC（libusb + libuvc）读取 USB 采集棒，输出 MJPEG 流
  - 动态枚举采集卡支持的分辨率/帧率，网页可切换
  - 软件重编码降码率（libjpeg-turbo + NEON），可调画质
  - 网页实测码率显示
- **键鼠（HID）**：FunctionFS 用户态 HID gadget，含
  - 键盘（boot protocol）
  - 相对鼠标
  - **绝对指针**（按画面坐标定位，解决黑边/偏移）
  - **Consumer Control**（多媒体/功能键：音量、播放/暂停、上一曲、下一曲、静音、计算器、邮件、主页等）
- **网页前端**：Canvas MJPEG 渲染、**自动黑边检测对齐**、指针锁定/绝对跟随、
  全屏、全键捕获（Keyboard Lock）、Ctrl+Alt+Del / Alt+Tab 按钮
  - **可显隐的虚拟键盘**：F1–F12 / PrtScn / ScrLk / Pause / 导航键 / 多媒体键；
    支持 Ctrl/Shift/Alt/Win 修饰键锁定
- **电源/复位跳线**：通过 GPIO 脉冲模拟被控机电源键/复位键（可配置、可持久化），
  支持状态检测脚
- **登录认证**：会话 Cookie + SHA-256(加盐) 口令；所有接口受保护
- **维护后台**：网页执行 shell（root）、查看系统状态、改配置、改密码、重启/关机

## 架构

```
被控机 HDMI ──USB采集棒──► RK3506 host口  ──libusb/libuvc(用户态)──► MJPEG HTTP
被控机 USB  ◄──OTG线────── RK3506 OTG口   ──FunctionFS HID─────────► 键鼠报文
GPIO(J4)    ────────────► 电源/复位键 (经三极管/光耦)
浏览器 ──HTTPS──► 网关 ──► 板子 :8080  (网页 / 视频 / 控制 / 后台)
```

两个独立进程：

| 进程 | 作用 | 监听 |
|---|---|---|
| `rkkvm-hid` | FunctionFS HID gadget + 行协议控制服务 | `127.0.0.1:5001` |
| `rkkvm-video` | UVC 采集 + MJPEG HTTP + 网页 + 认证 + 后台 + GPIO | `0.0.0.0:8080` |

`rkkvm-video` 通过本地 TCP 将控制指令转发给 `rkkvm-hid`。

## 目录结构

```
src/    hid_ffs.c      FunctionFS HID gadget 源码
        video_uvc.c    UVC 采集 + HTTP/网页/认证/后台/GPIO 源码
        gpioscan.c     GPIO 短路/跳线扫描器
deploy/ rkkvm.html     主界面    admin.html 维护后台    login.html 登录页
        S49kvmgpio.sh  开机配置跳线 GPIO
        S51rkkvm-hid.sh / S52rkkvm-video.sh  服务自启
        kvm.conf       GPIO 配置样例
        gpiotest.sh    单对 GPIO 短接检测
tools/  kvmctl.py      主机侧 HID 测试客户端
cmake-armv7-musl.cmake 交叉编译工具链文件
build.sh               一键交叉编译
```

## 编译

需要 Linux + `cmake`/`make`/`curl`。`build.sh` 会自动下载 Bootlin armv7-eabihf(musl) 工具链，
并交叉编译 libusb / libuvc / libjpeg-turbo（均静态），最后编译三个程序：

```bash
./build.sh
# 产物: src/rkkvm-hid  src/rkkvm-video  src/gpioscan
```

## 部署（到板子）

```bash
# 程序与网页
scp src/rkkvm-hid src/rkkvm-video root@<board>:/userdata/
scp deploy/rkkvm.html deploy/admin.html deploy/login.html root@<board>:/userdata/
scp deploy/kvm.conf root@<board>:/userdata/
# 启动脚本
scp deploy/S49kvmgpio.sh deploy/S51rkkvm-hid.sh deploy/S52rkkvm-video.sh root@<board>:/etc/init.d/
ssh root@<board> 'chmod +x /userdata/rkkvm-* /etc/init.d/S4* /etc/init.d/S5*'
```

访问：浏览器打开 `http://<board>:8080/`，未登录会跳转 `/login`。
**默认账号 `admin` / `admin`，请登录后在后台立即修改。**

### 让外网/其它网段访问（可选）
若板子在私有网段（如 192.168.50.0/24），可在网关上做端口转发，或用反向代理终止 TLS：

```
firewall-cmd --permanent --zone=public --add-forward-port=port=18080:proto=tcp:toport=8080:toaddr=<board>
```

或用 Caddy（提供 HTTPS，Keyboard Lock 需要安全上下文）：

```
https://<gateway>:18443 {
    tls internal
    reverse_proxy <board>:8080 { flush_interval -1 }
}
```

## HTTP 接口（均需登录）

| 路径 | 说明 |
|---|---|
| `/` `/admin` `/login` `/logout` | 界面 / 后台 / 登录 / 退出 |
| `/stream` `/snapshot` | MJPEG 流 / 单帧 JPEG |
| `/status` | 状态(分辨率/帧率/码率/GPIO/电源) |
| `/formats` | 采集卡支持的模式(JSON) |
| `/setres?w=&h=&fps=` | 切换分辨率/帧率 |
| `/setenc?mode=&quality=` | 直通 / 软件重编码 |
| `/gpio?n=&act=read\|high\|low\|pulse&ms=` | GPIO 控制 |
| `/gpiocfg?power=&reset=&status=` | 保存跳线 GPIO 配置 |
| `/ws` | WebSocket 低延迟键鼠控制 |
| `/admin/exec` (POST `cmd=`) | 后台执行 shell |

## GPIO 跳线（重要）

- TL3506-MiniEVM 的 **EXPORT(J2) 在 NAND 版上不可用作 GPIO**：其 FSPI 脚就是 SPI NAND 的信号，
  手册明确“NAND 版不可使用”，误接会崩甚至损坏数据。
- 可行方案：用 **UART0 调试口 J4**（pin1=3V3, pin2=GND, pin3/4 = UART0 TX/RX = `gpio22`/`gpio23`）
  复用为 GPIO 做开机/复位跳线（`/sys/class/gpio` 可导出，实测可用）。占用后调试串口控制台会停用。
- **务必经三极管/光耦隔离**再接到被控机电源键，勿直连。
- `gpioscan` / `gpiotest` 用于探测引脚、确认短接。

## 已知限制

- 需 `FunctionFS`、`libusb`(usbfs)、`configfs` 等内核支持（本板内核均有）。
- 运行期**不要解开 ffs gadget 的 UDC**：该内核在此操作上易挂起；更新程序后重启板子即可。
- 密码/会话：会话存于内存；重启后需重新登录。
- 维护后台以 root 执行任意命令，权限极大，请仅在可信网络使用，务必修改默认密码。

## 许可

未附带许可证文件；如需开源请自行添加（如 MIT）。
