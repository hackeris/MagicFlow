# 状态与后续(Status & Next)

> 2026-09-05 定稿。当前阶段:**核心目标已完成,处于"交付态整理"平台期**;本文记录收尾清单与后续推进方向(决策以本文件为准)。

## 1. 已完成(勿重复投入)

| 目标 | 证据/产物 |
|---|---|
| CPU 多线程推理(OpenBLAS 12 核) | MM4x=0.71s(0.61s 复现),`BLAS_INFO=open`;docs/local-inference-torch-parallel.md §6 |
| 装载全链修复(mbind/60 LAPACK/gfortran stub) | PROBE-DLOK;未定义符号差=0 |
| 真机出图 | 256×256 SD-Turbo ≈88s(字节级复现 103,411B) |
| **全链字节级复现** | docs/BUILD.md + thirdparty/ohos-torch/run_repro_chain.sh |
| NPU 可行性结论(定稿) | poc/npu/README.md(唯一权威;NPU 仅 9020+/9030+,本机 9000 系不合) |
| 设备能力上限定谳 | F32 模型 5.2G → 512 级 SIG9 OOM;**本机上限 256 级** |

## 2. 收尾小活(2026-09-05 已完成)

- [x] **临时资源归位**:`fp32_to_fp16.py` → `scripts/`;出图验证样板 workflow → `scripts/smoke_workflow_256x2.json`;`/tmp/g4_*.{json,png}` 已归档。
- [x] **备份收口**:`/data/share/comfy-ohos-port.old`(5.2G,完整旧研究区)**确认保留**未删(一次确认即可)。
- [x] **验证探针审计**:COMFTEST-BLAS/MM/MM4x 均在 `#ifndef RELEASE_BUILD` 段(Release 构建自动裁剪);生产配置 `OPENBLAS_NUM_THREADS=12`/`CORETYPE=ARMV8` 在 import 前正式段(非探针,保留)。
- [x] **设备侧收尾**:diag.log 已清理;当前设备事实——安装=复现链 HAP(39 号);模型=SD-Turbo **fp16**(2,607,364,064B,见 §3-① 试点结论);fport 8189→8188、rport 18000→18001(宿主 /tmp/models http.server 18001 常驻)。

## 2.5 fp16 试点结论(2026-09-05,终审)

**已执行**(工具:scripts/fp32_to_fp16.py,host 转换 12s→2.6GB→设备复用下载链):

| 规格(fp16 模型) | 结果 | 数据 |
|---|---|---|
| 256×256 steps=2 | ✅ 稳活 | 100s(含冷启动+载入),rss 峰值未触顶 |
| 448×448 steps=2 | ❌ SIG9 | 峰值 5.04G(瞬时飙至 6.3G),160s 被杀 |
| 512×512 steps=1 | ❌ SIG9 | **2 步推理已跑完(1:50)** 后 VAE decode 时刻峰值 5.17G 被杀 |
| 对照 fp32 256 | ✅ | 87-89s |

**结论**:
1. **fp16 收益确认**:模型常驻 5.2G→2.6G(省 2.6G)+存储减半;256 规格更稳;
2. **512 级仍不可行**——致命上限≈**5.0-6.3G(进程峰值)**(全局内存空 5.4G 仍 SIG9 ⇒ **非全局 OOM,平台级限制**(cgroup/调度器,shell 无权限读取,证据:同机型多轮一致);
3. **瓶颈链**:pyroot+torch 运行时基线 ≈2.3G + fp16 模型 2.6G = **4.9G 常驻**,任何大张量(448+ 激活/VAE decode)即触顶;
4. **最终定论:此机型(11.8G 物理)稳态规格 = 256 级(fp16 模型)**;512 需换大内存机型/或减小运行时基线(后续研究项,非紧急)。
5. 设备保留 fp16 模型(回退:宿主 `/tmp/models/sd_turbo_fp32.bak.safetensors`)。

## 3. 推进方向(待决策,按性价比)

| # | 方向 | 价值 | 成本/风险 | 优先级建议 |
|---|---|---|---|---|
| ① | fp16 模型试点 | **已完成**(见 §2.5):256 稳活、512 仍触顶;结论=稳态 256 级 | — | 关闭 |
| ② | workspace 门 + 延迟启动(对齐官方"创建/打开工作空间") | 中(产品观感) | 中:comfyui-src+frontend pin 升级有回归(patch/queue/探针链重验) | 排后 |
| ③ | CANN/AscendC 试点 | — | 需 9020+ 真机(HW 门槛) | 等硬件 |
| ④ | 一键 smoke(`make verify`:collect→install→出图→判据) | 中(防回归) | 低 | 顺手 |

## 4. 决策记录

- 2026-09-05:用户拍板"能跑就行,先把版本固定" → 512×4 判据明确**放弃**(记录在 local-inference-torch-parallel.md §7),不追 fp16 之前;
- 2026-09-05:复现链验证目标达成 → 归档为 BUILD.md;
- 官方新版(workspace 门户)行为与我 pin 的 0.34.0/1.54.1 版本代差,不属 bug(Index.ets onAppear 自动 launch 为 POC 自动化遗留,见第 3-②)。
