# A journey to a pure bitstream based video stabilization

[ENG](readme.md) | **中文**

能不能不看像素就完成视频稳像？`mvstab` 验证的是一个边界明确的答案：
对 H.264，只解码预测元数据，从编码器留下的最终运动矢量估计相机运动；
像素解码只留给无论如何都必须执行的最终渲染。

最终系统速度快，而且分析阶段完全基于码流。但实验也暴露了硬上限：编码运动是
有用证据，却不是光流。长时间序列分析能改善结果，面对困难运动时仍不如像素域
vid.stab。

[![八秒原视频、mvstab 与 vid.stab 对比](assets/mvstab-showcase.gif)](mvstab-comparison.mp4)

上方是原视频，左下是纯码流 mvstab，右下是像素域 vid.stab。点击 GIF 可观看
完整 6 分 19 秒对比，也可以[直接打开视频](mvstab-comparison.mp4)。

## 先看结果

测试视频是一段 H.264 Main profile 行车视频：720×480、29.97 fps、11,369 帧，
总长 379.35 秒。所有候选变换都使用相同的 `vidstabtransform` 参数渲染，再用
同一组 `vidstabdetect` 参数测量剩余运动。

| 检测器 | 平移中位数 | 平移 RMS | 平移 p95 | 旋转 RMS |
|---|---:|---:|---:|---:|
| 上一版纯 MV 基线（`e1fe820`） | 4.513 px | 10.187 px | 20.124 px | 0.751° |
| 最终长序列 mvstab | **4.211 px** | **9.881 px** | **19.739 px** | **0.717°** |
| 像素域 vid.stab | 2.283 px | 5.651 px | 8.014 px | 0.573° |

最终纯码流分析耗时 7.77 秒，常驻内存约 67 MB。`vidstabdetect` 墙钟时间为
12.71 秒，多线程累计 CPU 时间 246.57 秒，常驻内存约 129 MB。这里都只计算
运动分析，不包含双方共用的渲染和编码过程。

取舍很明确：在这段视频上，mvstab 的分析速度约快 1.6 倍，内存约为一半；
但剩余平移 RMS 比 vid.stab 高 75%，p95 长尾高 146%。

## 最初的想法

视频稳像可以拆成两个任务：

1. 估计相机运动；
2. 解码、变换、裁剪并重新编码图像。

第二步一定需要像素，第一步未必。帧间编码器已经选择并写入了一套运动预测，
或许检测器可以直接读取它，省掉额外的像素域分析。

这并不是“全流程永远不解码像素”。最终渲染仍会解码每一帧。这里的主张更窄，
也更实际：**运动分析可以只读取 H.264 语法，不重建亮度或色度像素。**

## H.264 到底保存了什么

H.264 把图像分成预测分区。分区可以帧内编码，也可以从一张或两张参考图像预测。
显式编码的帧间分区使用显式或推导出的参考索引，并携带运动矢量差 `MVD`。
最终矢量的概念式为：

```text
final_mv(分区, 参考) = predicted_mv(邻居, 参考) + MVD
```

预测值依赖邻近分区、分区形状、参考身份、slice 边界和场模式。Skip/direct 分区
会在没有显式索引或 MVD 的情况下推导部分或全部参考及运动值；B-direct 还可能
使用另一张图像中的同位块。因此，只读取 MVD 是错的：MVD 是编码差值，不是
块运动。

真正有用的观测在解码器完成规范规定的 MV 和参考推导之后出现：

```text
分区矩形 + 最终 MV + 精确参考图像 + 预测模式
```

四分之一像素精度的亮度 MV、从 16×16 到 4×4 的分区、list 0/list 1、长期
参考、P-skip、B-skip 和 B-direct，都必须按照解码器的真实规则处理。

## 第一次尝试：使用 FFmpeg 公共运动矢量

标准 FFmpeg 可以导出 `AV_FRAME_DATA_MOTION_VECTORS`。它适合做原型，却丢失了
H.264 slice 局部的精确参考。`source` 字段通常只能表达“较早”或“较晚”的预测
方向。

这不足以做时间归一化。`ref_idx_l0 = 0` 不是“上一帧”，而是当前 slice 活跃
参考列表中的第 0 项。重排序、B 帧和长期参考都会破坏这个简单假设。

在测试视频上，旧的仅符号 safe 估计器只有 7 帧产生了有意义的非零测量。
问题不是缺少运动矢量，而是丢失了参考身份和真实时间跨度。

## 不重建像素，提取精确运动

仓库中的 FFmpeg 补丁在 H.264 解码器仍持有 slice 局部参考列表时，导出每个最终
预测分区。记录内容包括精确 POC 和 PTS 差、列表及索引、长期参考、场状态，
以及 skip/direct 来源。

补丁后的解码器仍然要完成熵解码、宏块解析、最终 MV 推导，但主动跳过产生像素
的阶段：

- 反量化和反变换；
- 运动补偿像素重建；
- 帧内像素预测；
- 去块滤波。

CABAC 的残差符号仍必须消费，因为后续熵编码上下文依赖前面的语法。“纯码流”
表示“不重建图像”，并不表示可以在压缩比特中任意跳读。

这个路径从测试视频导出了 20,715,192 条预测记录。完整解码和仅元数据解码在
所有帧上得到逐字节相同的运动导出。单独解码耗时从 12.12 秒降至 3.28 秒，
加速 3.7 倍。

## 参考时间也是测量的一部分

跨一帧的矢量和跨四帧的同值矢量，并不代表相同速度。设当前时间为 `t_i`，
精确参考时间为 `t_r`，标称输出间隔为 `Δt`，mvstab 对编码位移 `m` 做如下
归一化：

```text
v = m / (t_i - t_r)
d_one_frame = v · Δt
```

符号直接来自真实时间差，因此同一个公式可以处理过去参考、未来参考、B 帧以及
可变帧时长。加入精确时间后，11,369 帧中有 11,088 帧得到非零运动。

## 从块预测得到相机运动

编码 MV 不是相机轨迹。纹理丰富的路面可能产生数千个小分区，天空却几乎没有；
一辆汽车也可能拥有高度一致的矢量场。零 skip 可能表示静态匹配良好，也可能只是
方向不可观测。

对每条精确参考边，mvstab 先在 8×4 图像网格中平衡证据，再以图像中心为原点
拟合仿射场：

```text
dx(x,y) = tx + a·x + b·y
dy(x,y) = ty + c·x + d·y

scale = (a + d) / 2
theta = (c - b) / 2
```

`tx`、`ty` 和 `theta` 是用于稳像的全局相似运动。额外的仿射项吸收非等比拉伸
和剪切，减少它们对平移和旋转的污染。四轮 Tukey 风格迭代重加权最小二乘用于
抑制空间离群点：

```text
w_robust(r) = (1 - (r / τ)²)²,  r < τ
              0,                r ≥ τ
```

阈值 `τ` 由残差中位数和配置的下限共同决定。模型还必须覆盖足够大的图像范围，
并在图像相对的两侧都有支持。即使一个紧密小区域内部非常一致，也不能被当作
全局相机运动。

## 其他码流信息带来的结论

运动矢量是唯一直接的位移观测。流水线还使用了一小组经过实际测试的 H.264 信息：
有些由补丁导出给估计器，有些来自常规帧元数据，还有些只是解码器正确推导导出值
所必需的上下文。

| 本次实验实际使用的信息 | 流水线角色与测量结论 |
|---|---|
| 精确参考和 PTS | 补丁导出及估计器输入。必不可少：它把稀疏、近似的方向恢复成时间正确的 P/B 帧约束。 |
| 分区形状 | 补丁导出及估计器输入。有用，但必须限制面积权重并做网格平衡，避免纹理区支配拟合。 |
| Skip/direct 模式 | 补丁导出及估计器输入。可作为弱证据；mvstab 会降权并拒绝精确零 skip。 |
| 多参考 | 补丁导出及估计器输入。只有作为独立、精确定时的边才有用；混合会产生错误平均。 |
| 关键帧状态 | 常规帧元数据。它是缺失观测，不一定是切镜；两侧连续时历史可以跨越 I 帧。 |
| Slice 边界 | 只作为解码器上下文。正确推导预测值和参考所必需，但不会被当成独立运动测量。 |

我们也考虑过以下信号，但当前补丁**没有**把它们导出给估计器，因此本次实验并未
验证其效果：

| 未测试的信号 | 可能提供的信息 |
|---|---|
| 帧内块覆盖率 | 一帧中有多少区域缺少帧间证据；仍不能恢复方向。 |
| 原始 MVD | 单独价值很小：最终 MV 已组合预测值和 MVD，而原始 MVD 不是物理运动。 |
| 残差能量、coded-block pattern、QP | 预测代价或量化强度；仍不能补出缺失的运动方向。 |
| 包大小 | 混合运动、残差、头部和码率控制决策的粗粒度复杂度提示。 |

反复出现的结论是“可观测性”：元数据能告诉我们估计很弱、来自继承、属于局部
对象或根本缺失；如果编码器没有保存位移，它就无法凭空创造方向。

## 跨关键帧的长序列分析

单帧鲁棒拟合仍会把持续移动的前景误认为相机运动。最终版本为每个占用的 8×4
网格单元保留紧凑运动摘要，并在 31 帧窗口中沿邻近单元追踪残差方向。

只有满足以下条件，网格单元才会被判定为持续前景：

- 去除相机运动后，每标称帧至少移动 1.5 像素；
- 至少与另外八次观测一致；
- 与同类单元合计不超过有效单元的四分之一；
- 至少留下六个单元用于背景拟合。

随后，相关参考边会排除这些单元并重新拟合。真正的全局相机冲击会影响大范围，
所以不会因为这个紧凑前景规则被删除。

孤立关键帧没有帧间块，因此被视为缺失样本。当关键帧两侧的平移、旋转、尺度和
时间戳连续时，历史可以跨越它；出现不连续则开启新分段，并禁止跨切镜插值。

它带来了真实但有限的改善：平移 RMS 从 10.187 降到 9.881 像素，旋转 RMS
从 0.751° 降到 0.717°。长序列更容易判断前景归属，却无法在帧内编码区域或
平坦区域创造新信息。

## 用位姿图连接多个参考

从参考帧 `r` 到当前帧 `i` 的精确预测，会成为相机位姿四个坐标——X、Y、旋转、
尺度——上的约束：

```text
pose[i] - pose[r] ≈ edge[r → i]
```

稀疏最小二乘按照置信度、空间覆盖率以及时间跨度平方的倒数为边加权。密集的相邻
边构成主图；长边可以连接真正稀疏的序列，但会被保守接纳，因为编码器选择参考
是率失真决策，不是相机模型。互不连通的图分量分别锚定。

mvstab 输出相对变换；FFmpeg 的 `vidstabtransform` 负责真正的轨迹平滑、裁剪
策略和图像变换。这样可以让检测器比较聚焦在运动估计本身。

## 对比实验如何完成

源视频是[这段 YouTube 行车视频](https://www.youtube.com/watch?v=SVA2mq9l2X8)。
它本身已经是 H.264，因此分析前没有转码。

mvstab 和 vid.stab 分别为同一源视频生成变换。两者使用相同的平滑参数且关闭
自动缩放进行渲染，再用以下参数测量剩余运动：

```text
shakiness=5, accuracy=15, stepsize=6, mincontrast=0.25
```

随后使用不做平滑的 debug pass 把检测结果转换成可比的全局运动。它不是绝对
真值——vid.stab 本身也是估计器——但一致的残差协议能显示每种稳像之后还剩下
多少运动。

![mvstab 与 vid.stab 全视频运动曲线](assets/mvstab-motion-plot.png)

## 最终差距

与 vid.stab 相比，最终纯码流 mvstab 的：

- 剩余平移中位数高 84%；
- 平移 RMS 高 75%；
- 平移 p95 高 146%；
- 旋转 RMS 高 25%。

长尾是核心弱点。编码预测为率失真优化，不为物理运动优化。编码器可能选择较远
参考、复用零矢量、把大量细节花在移动汽车上，或者在平坦区域几乎不留下证据。
像素域跟踪能直接观察图像内容并自行选择特征；纯码流检测器无法恢复编码器没有
保留的观测。

因此，这个实验成功证明了加速路径，但没有成为 vid.stab 质量的替代品。当目标
是避免检测阶段的像素解码时，纯码流分析快速、确定且有价值；追求最佳稳像质量
时，重建图像提供的证据仍然更强。

## 构建与运行

需要安装 CMake、C11 编译器、`pkg-config`，以及 `libavformat`、`libavcodec`、
`libavutil` 的开发包。构建补丁提取器还需要 Git、Make 和常规 FFmpeg 构建工具链。
对比脚本需要 Python 3，绘图还需要 Matplotlib。

如需使用标准 FFmpeg 回退路径，可以直接构建。这个路径会在解码器内部重建像素，
参考时间也是近似值，但不需要 FFmpeg 补丁：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

辅助脚本会构建固定版本的 FFmpeg，并应用仓库中的 H.264 补丁。它是有意裁剪的
最小分析版本，不包含 `vidstabtransform`、libx264 或常规 MP4 muxer：

```sh
scripts/build_patched_ffmpeg.sh /tmp/mvstab-ffmpeg /tmp/mvstab-ffmpeg-install

PKG_CONFIG_PATH=/tmp/mvstab-ffmpeg-install/lib/pkgconfig \
  cmake -S . -B build-patched -DCMAKE_BUILD_TYPE=Release \
    -DMVSTAB_REQUIRE_EXACT_METADATA_TESTS=ON
cmake --build build-patched -j

LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  ctest --test-dir build-patched --output-on-failure
```

使用补丁库进行分析：

```sh
LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab analyze input.mp4 \
    --mode safe -o motion.trf --stats motion.csv
```

渲染必须使用**另一套完整 FFmpeg**，其中需要包含 `vidstabtransform`、libx264
和目标 muxer。下面是日常观看的命令，会启用自动缩放；基准测试会关闭它：

```sh
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=motion.trf:relative=1:smoothing=30:optzoom=1" \
  -c:v libx264 -crf 18 -c:a copy stabilized.mp4
```

必须设置 `relative=1`。变换文件使用逆平移和经过 vid.stab 标定的角度符号。
尺度目前只作为干扰参数估计，不会写入 zoom。

### 复现实验对比

报告中的实验使用补丁库执行 mvstab 分析；vid.stab 检测、残差检测、所有渲染和
debug pass 共用另一套完整 FFmpeg。先下载并检查源视频：

```sh
yt-dlp -f "bv*+ba/b" --merge-output-format mp4 \
  -o 'source.%(ext)s' "https://www.youtube.com/watch?v=SVA2mq9l2X8"

ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,profile,width,height,r_frame_rate \
  -of default=noprint_wrappers=1 source.mp4
```

本次下载已经是 H.264。如果未来下载到其他编码，只转换视频流后再使用精确提取器：

```sh
ffmpeg -i source.mp4 -map 0:v:0 -map 0:a? \
  -c:v libx264 -preset medium -crf 18 -c:a copy input.mp4
```

如果已经是 H.264，把 `source.mp4` 重命名或链接为 `input.mp4`。然后生成两份变换：

```sh
LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab analyze input.mp4 \
    --mode safe -o mvstab.trf --stats mvstab.csv

ffmpeg -i input.mp4 \
  -vf "vidstabdetect=result=vidstab.trf:shakiness=5:accuracy=15:stepsize=6:mincontrast=0.25" \
  -f null -
```

使用相同平滑参数和 `optzoom=0` 渲染：

```sh
ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=mvstab.trf:relative=1:smoothing=30:optzoom=0" \
  -c:v libx264 -preset medium -crf 18 -an mvstab-output.mp4

ffmpeg -i input.mp4 \
  -vf "vidstabtransform=input=vidstab.trf:relative=1:smoothing=30:optzoom=0" \
  -c:v libx264 -preset medium -crf 18 -an vidstab-output.mp4
```

再用相同检测参数测量两份渲染输出：

```sh
ffmpeg -i mvstab-output.mp4 \
  -vf "vidstabdetect=result=mvstab-residual.trf:shakiness=5:accuracy=15:stepsize=6:mincontrast=0.25" \
  -f null -

ffmpeg -i vidstab-output.mp4 \
  -vf "vidstabdetect=result=vidstab-residual.trf:shakiness=5:accuracy=15:stepsize=6:mincontrast=0.25" \
  -f null -
```

为了得到可比较的全局运动，需要在彼此独立的空目录中，以不做平滑的
`vidstabtransform` debug 输出转换检测结果：

```sh
mkdir mvstab-residual-debug
cd mvstab-residual-debug
ffmpeg -i ../mvstab-output.mp4 \
  -vf "vidstabtransform=input=../mvstab-residual.trf:relative=1:smoothing=0:optzoom=0:debug=1" \
  -f null -
cd ..

mkdir vidstab-residual-debug
cd vidstab-residual-debug
ffmpeg -i ../vidstab-output.mp4 \
  -vf "vidstabtransform=input=../vidstab-residual.trf:relative=1:smoothing=0:optzoom=0:debug=1" \
  -f null -
cd ..
```

汇总两份残差全局运动文件。平移按 `sqrt(x² + y²)` 计算，p95 使用 nearest-rank
定义；旋转从弧度转换为角度后再计算 RMS：

```sh
python3 tools/summarize_motion.py \
  mvstab-residual-debug/global_motions.trf \
  vidstab-residual-debug/global_motions.trf
```

如需比较 mvstab 原始运动与 vid.stab 并重建仓库中的曲线图，先生成 vid.stab
全局运动文件，再运行对比工具：

```sh
mkdir vidstab-global
cd vidstab-global
ffmpeg -i ../input.mp4 \
  -vf "vidstabtransform=input=../vidstab.trf:relative=1:smoothing=0:optzoom=0:debug=1" \
  -f null -
cd ..

python3 tools/compare_vidstab.py \
  --codec mvstab.csv --vidstab vidstab-global/global_motions.trf

python3 tools/plot_motion.py \
  --codec mvstab.csv --vidstab vidstab-global/global_motions.trf \
  -o assets/mvstab-motion-plot.png
```

元数据检查命令：

```sh
LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab inspect input.mp4 --frame 217

LD_LIBRARY_PATH=/tmp/mvstab-ffmpeg-install/lib \
  build-patched/mvstab dump input.mp4 --format csv -o vectors.csv
```

## 编解码器支持

- **精确、无像素分析：**使用仓库补丁的软件 H.264 解码器。
- **近似回退：**能够导出 `AV_FRAME_DATA_MOTION_VECTORS` 的软件解码器；参考
  时间精度取决于具体解码器。
- **尚无精确补丁路径：**HEVC、VP9、AV1、硬件解码器，以及不导出最终 MV 的
  解码器。

容器格式不会统一规定运动元数据。每种编解码器都有自己的分区、参考列表、精度、
merge/skip/direct 规则和解码器数据结构。支持新编解码器，需要在最终运动和精确
参考同时存在的位置实现对应格式的导出器。

## 限制与仓库入口

- 分析不重建像素，但补丁解码器仍会分配帧缓冲并解析残差语法。
- 元数据模式关闭了损坏码流的错误隐藏。
- 输出是一组全局相似变换，不能修正视差、透视、滚动快门或网格运动。
- 生产级镜头切分，以及在切镜处重置外部 vid.stab 平滑，仍是后续工作。
- 最终渲染始终需要一次正常的像素解码。

解码器审计、补丁契约、算法、验证和 API 边界见 [design.md](design.md)。最初的
方案保存在
[mvstab_ffmpeg_codec_motion_vector_design.md](mvstab_ffmpeg_codec_motion_vector_design.md)。
