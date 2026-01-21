## Spreadtrum firmware dumper

使用官方SPRD U2S Diag驱动程序或LibUSB驱动程序。

### Windows预构建程序下载 [Release](https://github.com/TomKing062/spreadtrum_flash/releases)|[Dev](https://nightly.link/TomKing062/spreadtrum_flash/workflows/build/main)

### [原作者(ilyakurdyukov)版本的说明信息](https://github.com/ilyakurdyukov/spreadtrum_flash)

### 使用方法

此版本会对所有检测到的设备批量执行固定流程：

```
fdl fdl1-sign.bin 0x5500
fdl fdl2-sign.bin 0x9efffe00
exec
w splloader splloader.bin
w vbmeta vbmeta.img
w boot boot.img
reset
```

运行方式：

```
spd_dump [选项]
```

#### 选项

- `--wait <秒数> [数量]`

  指定等待设备连接的时间。设置数量后，需要检测到指定数量的设备才会继续。

- `--verbose <等级>`

  设置屏幕日志的详细程度（支持0、1或2，不影响文件日志）。

- `-h|--help|help`
  显示使用帮助。

#### 说明

该版本不再提供交互式命令或运行时命令，程序会按固定流程批量处理所有已连接设备。

### Android(Termux)

1. 安装[Termux-api](https://github.com/termux/termux-api/releases)并授权自启动

2. 安装依赖库和编译组件

```
pkg install termux-api libusb clang git
```

3. 拉取源代码

```
git clone https://github.com/TomKing062/spreadtrum_flash.git
cd spreadtrum_flash
```

4. 编译

```
make
```
生成可执行文件: spd_dump

5. 搜索OTG设备

```
termux-usb -l
[
"/dev/bus/usb/xxx/xxx"
]
```

6. 授权OTG设备(如果可用)

```
termux-usb -r /dev/bus/usb/xxx/xxx
```
允许访问目标设备

7. 运行 SPD_SUMP

```
termux-usb -e './spd_dump --usb-fd' /dev/bus/usb/xxx/xxx
```
