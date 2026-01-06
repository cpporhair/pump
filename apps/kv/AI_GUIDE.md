# KV 子系统帮助文档（面向 AI 智能体）

本文档用于快速理解 `apps/kv` 的代码结构、数据流和扩展路径，便于实现新功能或复用为类似项目。

---

## 1. 目标与架构概览

该 KV 子系统基于 SPDK（NVMe）与 `pump::sender` 异步管线模型实现，核心思路是：

- **写入**：在批次（batch）中收集 KV → 分配写入序列号（snapshot）→ 生成写入页 → 通过 NVMe 写入 → 更新内存索引 → 发布 snapshot。
- **读取**：基于当前可读 snapshot 从内存索引查找 → 若数据页未在内存则触发 NVMe 读取 → 返回 `key_value`。
- **调度**：每类功能（batch/index/fs/nvme/task）有独立 scheduler，使用 SPDK ring + poll 的事件驱动模型。

入口程序见 `apps/kv/main.cc`，核心启动流程在 `apps/kv/senders/start_db.hh`。

---

## 2. 目录导览（核心文件）

### 入口与流程编排
- `apps/kv/main.cc`：启动 DB，执行 YCSB load/update 等流程。
- `apps/kv/senders/start_db.hh`：解析配置、初始化 SPDK、构建 scheduler、启动运行循环与结束收尾（保存索引）。
- `apps/kv/senders/start_batch.hh` + `apps/kv/senders/finish_batch.hh`：批次生命周期管理。

### 数据结构与持久化
- `apps/kv/data/data_page.hh`：数据页与数据文件结构，定义读取/写入布局。
- `apps/kv/data/kv.hh`：`key_value` 的生命周期与构造。
- `apps/kv/data/index.hh` + `apps/kv/data/b_tree.hh`：基于 `absl::btree_map` 的主索引。
- `apps/kv/data/batch.hh` + `apps/kv/data/snapshot.hh`：批次数据与快照管理。
- `apps/kv/data/slots.hh`：SSD 页分配器（slot manager）。

### 调度器与执行模型
- `apps/kv/task/scheduler.hh`：基础 task/timer 调度器。
- `apps/kv/batch/scheduler.hh`：批次 snapshot 分配、发布。
- `apps/kv/index/scheduler.hh`：索引更新、缓存、读取。
- `apps/kv/fs/scheduler.hh`：页分配/回收、元数据页管理。
- `apps/kv/nvme/sender.hh`：NVMe 写入/读取任务调度。

### API 级 senders
- `apps/kv/senders/put.hh` / `apps/kv/senders/get.hh` / `apps/kv/senders/scan.hh` / `apps/kv/senders/apply.hh`：KV 操作管线。
- `apps/kv/senders/stop_db.hh`：停止运行。

---

## 3. 启动流程（start_db）

入口：`apps/kv/senders/start_db.hh` → `start_db(argc, argv)`

步骤概要：

1. **读取配置**：`runtime::read_config_from_file`（见 `apps/kv/runtime/config.hh`）。
2. **初始化 SPDK 环境**：`init_proc_env` → `spdk_env_init`。
3. **初始化 NVMe**：`init_ssd`（枚举设备、分配 qpairs）。
4. **初始化索引**：`init_indexes`（如果存在 `index.db`，则恢复索引）。
5. **初始化页分配器**：`init_allocator` + `init_pages`。
6. **初始化 scheduler**：`init_schedulers`。
7. **运行调度循环**：各核心执行 `task_proc`，轮询处理任务。
8. **退出时保存索引**：`save_index` 将内存索引持久化到 `index.db`。

配置核心字段见 `apps/kv/runtime/config.hh`：
`batch/index/fs/io` 的核心绑定、NVMe qpair 数量、FUA 配置等。

---

## 4. 数据模型（data）

### 4.1 slice
`apps/kv/data/slice.hh`

```
slice {
  uint64_t len;   // 包含 len 自身长度
  char ptr[];     // 实际字节
}
```

`make_slice` 返回动态分配内存（需注意释放）。

### 4.2 data_file / data_page
`apps/kv/data/data_page.hh`

- **data_file**：多页组合的 KV 记录。
- **data_page**：单页载体，含 SSD 位置信息与 payload。
- **布局逻辑**：
  - `version` 写在偏移 0。
  - `key_pos()` / `val_pos()` 计算 key/val 区域偏移。
  - `write_key` / `write_val` 写入 key/value。

### 4.3 index + version
`apps/kv/data/index.hh`

- `index` 持有 key 与多个版本（按 serial number 倒序）。
- `version` 绑定 `data_file`，用于 MVCC 读取。
- `b_tree` 维护 key → index 的映射（每个 core 一个）。

### 4.4 batch 与 snapshot
`apps/kv/data/batch.hh` + `apps/kv/data/snapshot.hh`

- batch 收集本次写入的 `key_value`。
- snapshot 用于读写隔离：
  - `get_snapshot`：读取时固定版本。
  - `put_snapshot`：写入时递增版本。

### 4.5 分配器
`apps/kv/data/slots.hh`

`slot_manager` 管理每个 SSD 的页面分配与回收，`allocate(write_span_list, batch)` 生成写入 span。

---

## 5. 写入流程（put/apply/publish）

核心管线：`apps/kv/senders/put.hh` + `apps/kv/senders/apply.hh`

### 5.1 关键步骤
1. `put(key_value)` 将 KV 写入 batch cache。
2. `apply()`：
   - `check_batch` → 检查批次有效性，并配合 `visit()` 算子进行编译期分支展开。
   - `batch::allocate_put_id` → 分配 put snapshot。
   - `index::update` → 更新内存索引版本列表。
   - `fs::allocate_data_page` → 分配物理页并生成 `write_span_list`。
   - `nvme::put_span` → NVMe 写入。
   - `fs::allocate_meta_page` → 写元数据页（持久化页引用）。
   - `index::cache` → 将写入数据页加入缓存。
3. `finish_batch()`：
   - 若有 put_snapshot → `batch::publish` 让新 snapshot 生效。

### 5.2 失败处理
`apply()` 中任何写入失败会触发：
- `free_page_when_error` 归还页面。
- `ignore_all_exception` 防止抛出导致主流程崩溃。

---

## 6. 读取流程（get）

入口：`apps/kv/senders/get.hh`

1. 通过 `index::get(key, read_sn, last_free_sn)` 获取版本。
2. 返回类型分支：
   - `pager_reader_res`：页面未在内存，需要 NVMe 读取 → `nvme::get_page`。
   - `pager_waiter_res`：已有其他读取在进行，等待通知即可。
   - `not_fround_res`：返回空 `key_value`。
3. 完成 NVMe 读取后通知等待队列，返回 `key_value`。

注意：`data_file::has_payload()` 用于判定页面是否已加载到内存。

---

## 7. 扫描流程（scan）

入口：`apps/kv/senders/scan.hh`

- 并发遍历所有 index scheduler 对应的 btree。
- 每个 core 维护局部 `scan_env_per_core`，最后合并为有序结果。
- `start_scan(key)` + `next()` 组合实现分页扫描。

---

## 8. 调度模型（Schedulers）

各 scheduler 基于 SPDK ring queue 实现生产者/消费者模型：

- **task scheduler**：基础任务/计时器调度。
  - `apps/kv/task/scheduler.hh`
- **batch scheduler**：分配 snapshot、发布 batch。
  - `apps/kv/batch/scheduler.hh`
- **index scheduler**：更新索引、缓存、读取。
  - `apps/kv/index/scheduler.hh`
- **fs scheduler**：数据页与元数据页分配/回收。
  - `apps/kv/fs/scheduler.hh`
- **nvme scheduler**：NVMe 写入/读取调度。
  - `apps/kv/nvme/sender.hh`

每个 scheduler 都有 `advance()`，在 `task_proc` 中被循环调用。

---

## 9. 索引持久化

`start_db.hh` 中：

- 启动时读取 `index.db` 并恢复 key → page 的映射。
- 关闭时写回 `index.db`（临时文件 + 拷贝）。

注意：索引恢复只保存 **最新版本** 的页位置与序列号。

---

## 10. 扩展指引（新功能/新项目）

### 10.1 新增 KV 操作
建议路径：
1. 在 `apps/kv/senders/` 中增加新的 sender（组合现有 sender）。
2. 若需要新的数据结构或调度，放到 `apps/kv/data/` 或 `apps/kv/*/scheduler.hh`。
3. 如果需要新的 scheduler 类型：
   - 新建 scheduler 实现。
   - 更新 `apps/kv/runtime/scheduler_objects.hh`。
   - 在 `apps/kv/runtime/init.hh` 中初始化。

### 10.2 新增存储类型/设备
- 扩展 `runtime::nvme_config` 与 `runtime::init_ssd`。
- 若需要替换 NVMe 逻辑，关注 `apps/kv/nvme/*`。

### 10.3 新增一致性/事务语义
重点关注：
- `apps/kv/batch/snapshot_manager.hh` 的 publish 逻辑。
- `apps/kv/data/index.hh` 中 version 选择策略。

---

## 11. 关键注意事项

- **内存释放**：`data::slice` / `data::key_value` / `data_file::key()` 返回的内存需注意生命周期。
- **payload 缓存**：`data::data_page_cache` 只保存引用，超出上限会释放 payload。
- **删除语义**：`make_tombstone` 用固定长度 val 表示删除，但读取层未显式处理 tombstone。
- **索引恢复**：当前只恢复最新版本，不保留历史版本链。

---

## 12. 快速入口索引

- 启动与关闭：`apps/kv/senders/start_db.hh`, `apps/kv/senders/stop_db.hh`
- 写入流程：`apps/kv/senders/put.hh`, `apps/kv/senders/apply.hh`
- 读取流程：`apps/kv/senders/get.hh`
- 扫描流程：`apps/kv/senders/scan.hh`
- Batch/快照：`apps/kv/batch/scheduler.hh`, `apps/kv/batch/snapshot_manager.hh`
- 索引与缓存：`apps/kv/index/scheduler.hh`, `apps/kv/data/cache.hh`
- NVMe：`apps/kv/nvme/sender.hh`
