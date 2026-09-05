# 模型下载系统(W3)—— 协议与 catalog 定型

> 2026-09-05 依据 W3 计划(S2 实施前置)定型。目标:模型经官方前端"缺模型条目/浏览
> 对话框"一键下载入库,落点为**用户可见目录**`Download/app.hackeris.hium/models/`(ArkTS
> picker 获取,extra_model_paths.yaml `is_default: true` 使 core 首选该路径)。
> 实现:core patches/16-ohos-model-download.patch(server.py 下载端点)。

## 1. 端点协议(server.py `@routes` 闭包区;自动 `/api` 双注册)

**路由注册顺序硬性**:`catalog` 具名路由必须先于 `{task_id}` 通配注册
(aiohttp 首匹配)。

| 方法/路径 | 请求体 | 成功 | 错误 |
|---|---|---|---|
| `GET /models/download/catalog` | — | `200 {catalog:[...]}`(§3) | — |
| `POST /models/download` | `{url, directory, filename, sha256?}` | `202 {task_id, status:"pending"}` | 400 `INVALID_URL/INVALID_FILENAME/INVALID_DIRECTORY`;409 `ALREADY_EXISTS` |
| `GET /models/download` | — | `200 {tasks:[...]}`(含历史任务) | — |
| `GET /models/download/{task_id}` | — | `200 {task}` | 404 `NOT_FOUND` |
| `POST /models/download/{task_id}/pause` | — | `202 {status:"paused"}` | 409(非 in_progress/pending) |
| `POST /models/download/{task_id}/resume` | — | `202 {status:"in_progress"}` | 409(非 paused) |
| `POST /models/download/{task_id}/cancel` | — | `202 {status:"cancelled"}` | 409(completed 不可取消) |
| `DELETE /models/download/{task_id}` | — | `200` | 409(in_progress 不可删) |

### 状态机

```
pending → in_progress → completed
                    ├→ error      (terminal)
                    └→ cancelled  (terminal)
      in_progress ⇄ paused  (resume 从头重拉, 不做断点续传 —— 第一版语义)
```

- 任务字段:`task_id(uuid4().hex[:16])`, `url`, `directory`(逻辑类型名), `filename`,
  `dest_dir`(内部绝对路径), `status`, `bytes_received`, `bytes_total`, `speed_bps`
  (0.5s 采样 × 3 点滑动), `error{code,message}`, `created_at`, `updated_at`, `sha256`
- 并发:module 级 `_OHOS_DL_TASKS: dict` + `asyncio.Lock`;同一 (url, directory, filename)
  进行中任务 → 幂等复用返回;已完成 → 409
- staging:写入 `dest_dir` 内 `.`+`<filename>.ohosdl-<task8>.part`;完成且(可选)sha 校验
  通过 → `os.replace` 原子改名;cancel/error/finally 删 part;启动时清理陈旧 `.part`
- 目标已存在 `dest_dir/filename` → 409(绝不覆盖)

### 校验清单(POST 逐条,与前端 missingModelDownload.ts 常量逐字同步)

1. **url**:`urlparse`;scheme ∈ {https, http};host ∈ {huggingface.co, civitai.com,
   civitai.red, github.com, 127.0.0.1, localhost}(回环用于 rport 隧道/smoke);
2. **directory** ∈ `folder_paths.folder_names_and_paths`(**运行时判定**,避免人肉清单漂移;
   未知 key 会被 add_model_folder_path 安全注册 → 直接判在字典即可);ArkTS 侧 mkdir 最小 6 类
   (checkpoints/loras/vae/controlnet/upscale_models/embeddings),下载端点对 dest_dir 写前
   `os.makedirs(exist_ok=True)`;
3. **filename** = `os.path.basename`,无 `..`,≤128 chars,后缀 ∈
   {.safetensors, .sft, .ckpt, .pth, .pt};与 dest 组合后 realpath 仍在 dest_dir 内;
4. 已存在 → 409。

### 错误码

`INVALID_URL / INVALID_FILENAME / INVALID_DIRECTORY / ALREADY_EXISTS / NOT_FOUND / BUSY /
HTTP_401 / HTTP_403 / NETWORK / TIMEOUT / DISK_FULL / SHA_MISMATCH / CANCELLED`
(401/403 透传自 HF gated;前端 MissingModelRow 已有 gated 徽章展示)。

## 2. 下载实现

- **aiohttp(依赖集已有;复用 `PromptServer.setup()` 的 `self.client_session`,
  timeout=None + 单请求 `ClientTimeout(connect=15, sock_read=120)`)**;
  `async for chunk in resp.content.iter_chunked(64*1024)` 流式分块写盘;
- 速度采样:每 0.5s 记 `(t, bytes)`,3 点滑动 `speed_bps`;
- 磁盘满:`OSError(errno==28)` → DISK_FULL 并删 staging;
- 兜底:真机 aiohttp 纯 py 异常(<0.5MB/s 且 CPU 100%)→ 换 urllib+executor,只动 `_run`。

## 3. Catalog(静态 dict,GET catalog 返回)

| id | name | directory | url(primary) | size_bytes(以 HEAD 实测入档) | tier |
|---|---|---|---|---|---|
| sd-turbo | sd_turbo.safetensors | checkpoints | https://huggingface.co/stabilityai/sd-turbo/resolve/main/sd_turbo.safetensors | —(实测后填) | **required** |
| sd-vae-ft-mse | vae-ft-mse-840000-ema-pruned.safetensors | vae | https://huggingface.co/stabilityai/sd-vae-ft-mse-original/resolve/main/vae-ft-mse-840000-ema-pruned.safetensors | ~335MB | optional |
| sd15 | v1-5-pruned-emaonly.safetensors | checkpoints | https://huggingface.co/runwayml/stable-diffusion-v1-5/resolve/main/v1-5-pruned-emaonly.safetensors | ~4.27GB(标注大/慢) | optional |
| realesrgan-x4plus | RealESRGAN_x4plus.pth | upscale_models | https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth | 67,040,989(HEAD 实测) | optional |

- 字段:`{id, name, directory, url, size_bytes, description, tier}`
- **镜像 hook**:env `OHOS_CATALOG_MIRROR`(如 `http://127.0.0.1:18000`)存在时
  url = `MIRROR.rstrip('/') + '/' + basename(url)`;默认 HF 直链。
- **URL 缺失兜底(关键)**:设备无官方 metadata → 缺模型条目可能只有 name/directory 无 url;
  前端下载动作先 `fetchModelDownloadCatalog()` 按 name 匹配补 url,无匹配 → 不显示下载钮。

## 4. 验证口径(宿主冒烟/真机)

- 宿主:`cd externals/comfyui-src && python3 main.py --cpu --port 8190 --front-end-root ...`
  + 本地 `python3 -m http.server 18001` 供 url(或临时文件服务器);用例表(见 S2):
  POST 正常/重复幂等/409/400 各错误/GET 进度递增/pause/resume/cancel/删/DELETE in_progress 409/
  sha 失败用例/目录存在不覆盖。
- 真机:见 W3 计划 §7 验证矩阵 Q0-Q5。
