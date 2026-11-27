# Turner 摄像头补丁

内核补丁 0001 至 0003 按顺序接在既有 Turner Linux v7.3 移植序列之后。I²C AP 通道和 MT6373 供电修改进入原有驱动。摄像头模块继续单独构建。在完整内核构建并生成 Module.symvers 后运行：

```sh
make -C "$linux" ARCH=arm64 O="$build" M="$linux/drivers/media/platform/mediatek/turner" TURNER_LIVE_CLOCK=1 modules
```

需要 V4L2、media controller、异步子设备和 RT6160 调压器支持。#156 的摄像头试验使用单独构建的 RT6160 模块。north、drive-fix 和 front-pins 针对本机原厂设备树。实测使用双核 RAM 桌面、按需绑定北侧 I²C 控制器；自动加载和整套冷启动仍待验证。

摄像头预览程序直接从用户态覆盖目录安装。KDE 普通用户运行 `turner-camera-preview /dev/video0` 拍摄后摄，或 `turner-camera-preview /dev/video1` 拍摄前摄。需要 Python 3、PyGObject、GTK 3 和 GdkPixbuf。

CAMSV 使用两个独立 DMA 地址。帧完成中断记录时间戳，立即把空闲地址提交给硬件；工作线程同时读取刚完成的缓冲。消费者来不及时，驱动只在缓冲释放后继续入队。STREAMOFF 停止入队、关闭传感器并排空 DMA。内核需要 videobuf2-v4l2 和 videobuf2-vmalloc；安装匹配的相机及 RT6160 模块后运行 depmod。

两个标准 V4L2 节点支持输入、格式、尺寸和固定帧间隔枚举。YUYV 4:2:2 提供 640×480 和 1280×720，均为 30 fps。原生尺寸提供 BGGR8 和 BGGR10。后摄 2104×1184 为 60 fps，4208×3120 为 30 fps；前摄 3264×2448 为 30 fps。同一时刻选择一路摄像头。

用 FFmpeg 录制可直接播放的 H.264 文件，前摄将设备改为 /dev/video1：

```sh
ffmpeg -f v4l2 -input_format yuyv422 -video_size 1280x720 -framerate 30 -i /dev/video0 -t 5 -c:v libx264 -preset ultrafast -pix_fmt yuv420p rear.mkv
```

`turner-camera-record --full --seconds 5 rear.avi` 保存后摄全幅 MJPEG。去掉 `--full` 使用后摄 custom1，使用 `--device /dev/video1` 选择前摄，`--width` 等比缩小。Debian KDE 安装 FFmpeg；工具优先使用其原生编码器，旧 BG10 环境保留 Python、libswscale 与 GdkPixbuf 路径。
