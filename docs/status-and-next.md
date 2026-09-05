# 状态与后续(Status & Next)

> 2026-09-05 定稿。当前阶段:**核心目标已完成,处于"交付态整理"平台期**;本文记录收尾清单与后续推进方向(决策以本文件为准)。

## 1. 已完成(勿重复投入)

| 目标 | 证据/产物 |
|---|---|
| CPU 多线程推理(OpenBLAS 12 核) | MM4x=0.71s(0.61s 复现),`BLAS_INFO=open`;docs/local-inference-torch-parallel.md §6 |
| 装载全链修复(mbind/60 LAPACK/gfortran stub) | PROBE-DLOK;未定义符号差=0 |
| 真机出图 | 256×256 SD-Turbo ≈88s(字节级复现 103,411B) |
| **全链字节级复现** | docs/BUILD.md + thirdparty/ohos-torch/run_repro_chain.sh |
| AscendC 调研结论 | docs/ascendc-npu-acceleration.md(CANN Kit 仅 9020+,本机 9000 系不合) |
| 设备能力上限定谳 | F32 模型 5.2G → 512 级 SIG9 OOM;**本机上限 256 级** |

## 2. 收尾小活(待办,零风险,可顺手闭环)

- [ ] **临时资源归位**:`/tmp/fp32_to_fp16.py`(fp16 转换工具,512 的钥匙)→ 入库 `scripts/`;`/tmp/g4_*.{json,png}` 归档或删除。
- [ ] **备份收口**:`/data/share/comfy-ohos-port.old` 确认后用 rm 清理(或保留不删,一次确认)。
- [ ] **验证探针审计**:最终 HAP 在 `RELEASE_BUILD` 下探针(COMFTEST-BLAS/MM/MM4x)已裁剪的收尾验证(可选)。
- [ ] **设备侧收尾**:清理沙箱 `diag.log` 等;记录"当前设备/模型/端口转发(fport 8189/rport 18000)"运维事实(记入本文件或 README)。

## 3. 推进方向(待决策,按性价比)

| # | 方向 | 价值 | 成本/风险 | 优先级建议 |
|---|---|---|---|---|
| ① | **fp16 模型试点**(模型 5.2G→2.6G) | 高:512 级救活、出图内存/速度双赢 | 低:转换工具已备(`fp32_to_fp16.py`,仅 numpy+safetensors),30 分钟可试 | ⭐ 最值得,唯一不动架构可修复上限的路 |
| ② | workspace 门 + 延迟启动(对齐官方"创建/打开工作空间") | 中(产品观感) | 中:comfyui-src+frontend pin 升级有回归(patch/queue/探针链重验) | 排后 |
| ③ | CANN/AscendC 试点 | — | 需 9020+ 真机(HW 门槛) | 等硬件 |
| ④ | 一键 smoke(`make verify`:collect→install→出图→判据) | 中(防回归) | 低 | 顺手 |

## 4. 决策记录

- 2026-09-05:用户拍板"能跑就行,先把版本固定" → 512×4 判据明确**放弃**(记录在 local-inference-torch-parallel.md §7),不追 fp16 之前;
- 2026-09-05:复现链验证目标达成 → 归档为 BUILD.md;
- 官方新版(workspace 门户)行为与我 pin 的 0.34.0/1.54.1 版本代差,不属 bug(Index.ets onAppear 自动 launch 为 POC 自动化遗留,见第 3-②)。
