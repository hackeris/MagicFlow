# smoke 设计(独立与内聚,零产品代码侵入)

> 2026-09-05 设计稿(未实施)。依据用户两条标准:**① smoke 不侵入产品功能逻辑; ② smoke 逻辑内聚到一处**。

## 0. 现状侵入审计(改造对象)

| # | 侵入点 | 位置 | 问题 |
|---|---|---|---|
| 1 | COMFTEST torch/BLAS/MM/MM4x 探针段 | `comfy_child.cpp:1221-1248`(`#ifndef RELEASE_BUILD` 半门) | 测试代码嵌在产品 import 主流程;Release 依赖宏开关,debug 全暴露 |
| 2 | COMFTEST py/site 早期探针 | `comfy_child.cpp:1064-1067` | 同上 |
| 3 | `USE_PROBE_MAIN`/`PROBE_BRANCH` if/else | `CMakeLists.txt:187-208` | 双分支残根(probe_main 文件已删) |
| 4 | verify_smoke.sh 判据 A/B 依赖 grep 产品探针 | 脚本 `[2]/[3]` 段 | 判据与产品实现强耦合(产品改名/重构即断) |

## 1. 目标设计(纯黑盒:smoke 全部在系统外部)

```
┌─ smoke 单一居所 ──────────────────────────────────────────┐
│ scripts/verify_smoke.sh(全部判据与流程)                     │
│   ├─ 判据 0: 设备在线/HAP 存在                             │
│   ├─ 判据 1: POST ① OHOS_SmokeBench 工作流 → 后端执行       │
│   ├─ 判据 2(A/B): 从 /history outputs 的 smoke_result.json │
│   │    解析 BLAS=open 与 mm4x<2.0(执行方=comfy 侧节点)      │
│   └─ 判据 3(C): 正常出图 workflow(smoke_workflow_256x2)    │
│                                                     —— 全部经 8198 HTTP API      │
└─────────────────────────────────────────────────────────────┘
                     ▲ 唯一触碰点
comfyui-src patch:custom_nodes/ohos_smoke/  (独立自检节点)
   · register_class OHOS_SmokeBench: import torch → torch.__config__.show()
     → 2048²×4 matmul 计时(nthreads) → 写 {pyroot}/smoke_result.json
     → 输出 JSON(via /history outputs)  → 产品路径永不加载该节点
```

**取舍理由**:
- **产品代码零改动**(comfy_child.cpp / CMakeLists 清理到无测试分支);fire-to-fire 验证不必需编译期宏;
- smoke **单处内聚** = `scripts/verify_smoke.sh` + 一个自检节点(节点亦是 ComfyUI 生态标准形态,独立包);
- 自检节点走 ComfyUI 的 **custom_nodes 加载机制**——产品不引用即无行为变化;`make verify` 触发它只因为提交了包含该节点的 workflow。

## 2. 改造清单(实施时)

1. **删除**:`comfy_child.cpp` 1221-1248 + 1064-1067(COMFTEST 全部);**删除** `CMakeLists.txt` 187-208(USE_PROBE_MAIN if/else),编译产品单分支;
2. **新增**:comfyui-src patch `15-ohos-smoke-bench-node.patch`:`custom_nodes/ohos_smoke/custom_node.py`(CLASS_ID `OHOS_SmokeBench_BLASMM4x`;输入 seed(哑);输出 `smoke_result.json` 路径+副本;内含:config 收集、2048²×4 MM 计时、`torch.__config__.show()` 过滤 BLAS 行);
3. **更新**:verify_smoke.sh 判据 A/B 改为「POST smoke-bench 节点 workflow → 轮询 /history → 读 outputs 文件 → Python 判定」;判据 C 不变(已是外部);
4. 保留 `--fast`(只判 1+2);`COMFY_SMOKE=1` 是 workspace 门落地时"后端启动自动化"的 UI 侧开关(见 workspace-design W1),与判据无关。

## 3. 验收(改造完成后)

- [ ] `make verify` 5 判据全 PASS(与现状等价);
- [ ] 产品断言:`grep -rn "COMFTEST\|OHOS_SmokeBench" entry/src/main/cpp/` = 0;
- [ ] Release/Debug 构建产物都不含 smoke 符号(`strings libcomfy_child.so | grep COMFTEST` = 空);
- [ ] 出图基准不变(字节级 smoke_workflow_256x2,与基线比对 10KB 以上);
- [ ] 双区镜像 + BUILD.md §4 环节速查补一行(smoke 节点)。

## 4. 关联文档

- `docs/workspace-design.md` §2:验证模式(COMFY_SMOKE=1)在 UI 层接入本 smoke 判据;
- `docs/BUILD.md` §3:smoke 一节(已有)更新为上述黑盒判定。
