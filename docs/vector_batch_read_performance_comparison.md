# shm_next Vector 批量读性能对比

本文记录 `shm_next::SharedMemoryVector` 在“单次加锁读取多条数据”场景下的性能。测试目标是验证：此前单条 `at()` 低于 `shm::at()` 的问题，是否可以通过 `Synchronized<Vector, InterprocessMutex>` 减少锁次数解决。

## 测试代码

| 项目 | 测试文件 |
| --- | --- |
| `shm_next` 单条外部锁读取 | `test/container/compare/shm_vector_perf_compare.cpp` |
| `shm_next` 批量外部锁读取 | `test/container/compare/shm_vector_batch_read_perf_compare.cpp` |
| `shm` 单条内部锁读取 | `test/shm/object/VectorPerfCompareTest.cpp` |

## 测试口径

统一参数：

```text
workers=4
ops=50000
record_bytes=64
batch_size=256
```

稳定性复测口径：

```text
rounds=5
result=avg[min,max] ns/op
```

接口覆盖：

| 语义 | `shm_next` 批量读 | 对照项 |
| --- | --- | --- |
| 原地读取 | `Synchronized<Vector, InterprocessMutex>::with_lock()` 内连续调用 `at()` | `shm::at()` 单条内部锁 |
| 下标读取 | `Synchronized<Vector, InterprocessMutex>::with_lock()` 内连续调用 `operator[]` | `shm_next::operator[]` 单条外部锁 |
| 拷贝读取 | `Synchronized<Vector, InterprocessMutex>::with_lock()` 内连续 `operator[] + memcpy` | `shm::fetch()` |

说明：

- 该测试不改变 `SharedMemoryVector` API，而是通过 `interprocess/sync/synchronized.h` 提供的同步包装器验证更合理的调用方式。
- 对连续读场景，调用方应避免每条记录都单独加锁；一次锁内读取一段连续数据可以显著降低端到端成本。
- `batch_size=256` 表示每次进入临界区后连续读取最多 256 条记录。该口径用于衡量业务可分批读取时的锁摊销成本。

## 编译与运行方式

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target test_container_compare_shm_vector_batch_read_perf_compare

./build/test/test_container_compare_shm_vector_batch_read_perf_compare \
  --workers=4 \
  --operations=50000 \
  --batch-size=256
```

也可以通过 CTest 运行：

```bash
ctest --test-dir build -R test_container_compare_shm_vector_batch_read_perf_compare --output-on-failure
```

## 测试结果

| 场景 | `shm_next` 单条外部锁 avg[min,max] | `shm_next` 批量外部锁 avg[min,max] | `shm` 单条内部锁 avg[min,max] | 稳定性判定 |
| --- | ---: | ---: | ---: | --- |
| 多线程 `at` | 46.5 [39.9,67.2] ns/op | 10.1 [7.3,13.6] ns/op | 27.8 [22.7,38.2] ns/op | 批量读后 `shm_next` 稳定更快 |
| 多进程 `at` | 52.2 [41.4,58.1] ns/op | 14.9 [12.1,16.8] ns/op | 30.5 [25.9,35.1] ns/op | 批量读后 `shm_next` 稳定更快 |
| 多线程 `operator[]` | 44.5 [39.9,54.1] ns/op | 5.3 [4.3,5.7] ns/op | - | 批量读显著降低锁成本 |
| 多进程 `operator[]` | 52.5 [39.3,67.4] ns/op | 13.6 [9.5,18.0] ns/op | - | 批量读显著降低锁成本 |
| 多线程拷贝读取 | 51.5 [42.1,64.4] ns/op | 5.6 [5.0,7.3] ns/op | `fetch`: 1329.1 [1155.8,1595.2] ns/op | `shm_next` 优势稳定且进一步扩大 |
| 多进程拷贝读取 | 63.5 [50.6,81.7] ns/op | 30.9 [27.4,33.5] ns/op | `fetch`: 1391.8 [1249.0,1563.8] ns/op | `shm_next` 优势稳定且进一步扩大 |

## 结论

- `SharedMemoryVector::at()` 本身不是主要瓶颈；此前低于 `shm::at()` 的核心原因是每条记录都独立加外部锁。
- 在连续读取场景下，通过 `Synchronized<Vector, InterprocessMutex>::with_lock()` 批量读取可以让 `shm_next::at()` 反超 `shm::at()`。
- 对性能敏感的 vector 读路径，推荐调用方按批次进入 `with_lock()`；如果调用方已经保证边界安全，可在批量锁内使用 `operator[]` 获得更低成本。
- 该结果也证明 `InterprocessSharedMutex` 不是 vector 单条读取的优先优化方向，减少锁次数比替换锁类型更有效。
