# shm_next 与 shm Vector 性能对比

本文记录 `shm_next` 与参考项目 `shm` 的 vector 接口性能对比。测试只覆盖当前阶段关注的两类操作：

- 单条写入：`push_back`
- 单条读取：原地读取 `at`，以及拷贝读取 `operator[] + memcpy` / `fetch`

## 测试代码

| 项目 | 测试文件 |
| --- | --- |
| `shm_next` | `test/container/compare/shm_vector_perf_compare.cpp` |
| `shm` | `test/shm/object/VectorPerfCompareTest.cpp` |

## 测试口径

统一参数：

```text
workers=4
ops=50000
record_bytes=64
```

稳定性复测口径：

```text
rounds=5
result=avg[min,max] ns/op
```

判定规则：

- 如果更快一方的最大值仍低于另一方最小值，记为“稳定更快”。
- 如果两边区间重叠但平均值方向一致，记为“平均更快但区间重叠”。

锁口径：

| 项目 | 锁策略 |
| --- | --- |
| `shm_next` | 容器内部无锁，每次 vector 操作用外部 `InterprocessMutex` 保护 |
| `shm` | 使用容器内部锁，不额外叠加外锁 |

比较分组：

| 语义 | `shm_next` | `shm` |
| --- | --- | --- |
| 原地读取 | `at()` | `at()` |
| 拷贝读取 | `operator[] + memcpy` | `fetch()` |

`shm_next::operator[]` 本身是零拷贝引用访问，不能直接和 `shm::fetch()` 比较；因此增加 `operator[] + memcpy` 场景对齐 `fetch()` 的拷贝语义。

## 编译与运行方式

### shm_next

远程编译与运行：

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target shm_vector_perf_compare

./build/shm_vector_perf_compare --workers=4 --operations=50000
```

### shm

参考项目位于：

```bash
cd <remote-shm-reference-workspace>
```

远程编译与运行：

```bash
cmake -S . -B build \
  -DSHM_BUILD_TESTS=ON \
  -DFRAME_INCLUDE_OUTPUT_PATH=<remote-shm-reference-workspace>/build/frame_include \
  -DCMAKE_CXX_FLAGS="-std=c++17"

cmake --build build -j 4 --target gtshm20

./testbin/gtshm20 --gtest_filter="VectorPerfCompareTest.*" \
  --data_amount=50000 \
  --write_thread_num=4 \
  --read_thread_num=4 \
  --data_length=64
```

说明：

- 当前 `shm` 工程源码使用 `std::shared_mutex`，需要 C++17。
- 当前 `shm` 顶层默认不构建测试，测试构建通过 `SHM_BUILD_TESTS=ON` 打开。
- `shm` 测试 CMake 带 `-fprofile-arcs -ftest-coverage`，重复运行时可能输出 `libgcov profiling error ... overwriting an existing profile data with a different timestamp`。本轮 gtest 结果为通过，gcov 警告不影响本次性能数据读取。

## 测试结果

| 场景 | `shm_next` | `shm` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 3.831 ms / 76.6 ns/op | 8.008 ms / 160.2 ns/op | `shm_next` 更快，约 2.1x |
| 多进程 `push_back` | 9.506 ms / 190.1 ns/op | 9.403 ms / 188.1 ns/op | 基本持平，`shm` 略快 |
| 多线程 `at` | 2.257 ms / 45.1 ns/op | 1.462 ms / 29.2 ns/op | `shm::at` 更快 |
| 多进程 `at` | 2.295 ms / 45.9 ns/op | 1.430 ms / 28.6 ns/op | `shm::at` 更快 |
| 多线程拷贝读取 | `operator[] + memcpy`: 2.575 ms / 51.5 ns/op | `fetch`: 60.375 ms / 1207.5 ns/op | `shm_next` 明显更快 |
| 多进程拷贝读取 | `operator[] + memcpy`: 3.044 ms / 60.9 ns/op | `fetch`: 64.134 ms / 1282.7 ns/op | `shm_next` 明显更快 |

## 本轮复测结果

| 场景 | `shm_next` | `shm` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 3.464 ms / 69.3 ns/op | 9.001 ms / 180.0 ns/op | `shm_next` 更快，约 2.6x |
| 多进程 `push_back` | 9.058 ms / 181.2 ns/op | 11.255 ms / 225.1 ns/op | `shm_next` 更快，约 1.2x |
| 多线程 `at` | 2.339 ms / 46.8 ns/op | 1.174 ms / 23.5 ns/op | `shm::at` 更快，约 2.0x |
| 多进程 `at` | 2.716 ms / 54.3 ns/op | 1.365 ms / 27.3 ns/op | `shm::at` 更快，约 2.0x |
| 多线程拷贝读取 | `operator[] + memcpy`: 2.607 ms / 52.1 ns/op | `fetch`: 63.183 ms / 1263.7 ns/op | `shm_next` 更快，约 24.2x |
| 多进程拷贝读取 | `operator[] + memcpy`: 3.812 ms / 76.2 ns/op | `fetch`: 66.696 ms / 1333.9 ns/op | `shm_next` 更快，约 17.5x |

## 5 轮稳定性复测结果

| 场景 | `shm_next` avg[min,max] | `shm` avg[min,max] | 稳定性判定 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 75.9 [64.7,93.0] ns/op | 161.0 [135.2,183.3] ns/op | `shm_next` 稳定更快 |
| 多进程 `push_back` | 234.8 [144.4,352.0] ns/op | 200.1 [176.3,245.9] ns/op | `shm` 平均更快但区间重叠，整体接近且波动较大 |
| 多线程单条 `at` | 46.5 [39.9,67.2] ns/op | 27.8 [22.7,38.2] ns/op | `shm` 稳定更快 |
| 多进程单条 `at` | 52.2 [41.4,58.1] ns/op | 30.5 [25.9,35.1] ns/op | `shm` 稳定更快 |
| 多线程批量 `at` | 10.1 [7.3,13.6] ns/op | 27.8 [22.7,38.2] ns/op | `shm_next` 稳定更快 |
| 多进程批量 `at` | 14.9 [12.1,16.8] ns/op | 30.5 [25.9,35.1] ns/op | `shm_next` 稳定更快 |
| 多线程单条拷贝读取 | 51.5 [42.1,64.4] ns/op | `fetch`: 1329.1 [1155.8,1595.2] ns/op | `shm_next` 稳定更快 |
| 多进程单条拷贝读取 | 63.5 [50.6,81.7] ns/op | `fetch`: 1391.8 [1249.0,1563.8] ns/op | `shm_next` 稳定更快 |
| 多线程批量拷贝读取 | 5.6 [5.0,7.3] ns/op | `fetch`: 1329.1 [1155.8,1595.2] ns/op | `shm_next` 稳定更快 |
| 多进程批量拷贝读取 | 30.9 [27.4,33.5] ns/op | `fetch`: 1391.8 [1249.0,1563.8] ns/op | `shm_next` 稳定更快 |

## 优化项与推荐用法

本轮原始单条读取中，`shm_next::at()` 低于 `shm::at()`。原因不是 `SharedMemoryVector::at()` 本身成本高，而是 `shm_next` 容器内部无锁，测试为保证并发安全对每条读取都单独进入外部 `InterprocessMutex`。

已落地的优化技术：

| 优化技术 | 作用 | 适用接口 |
| --- | --- | --- |
| `Synchronized<SharedMemoryVector<T>, InterprocessMutex>::with_lock()` | 单次进入临界区后连续读取多条记录，减少锁次数 | `at()`、`operator[]`、`operator[] + memcpy` |
| 批量连续读 | 将每条记录一次锁改为每个 worker 一段连续区间一次锁 | 连续读取 / 扫描类场景 |
| 保持 `operator[]` 热路径 | 调用方已保证边界安全时，避免 `at()` 的边界检查 | 已验证边界的热点读 |

`InterprocessSharedMutex` 也做过单独验证，但对 vector 极短单条读并不是优先优化方向。远程数据表明，减少锁次数比替换锁类型更有效。

## 优化后汇总结果

| 场景 | 推荐 `shm_next` 用法 | `shm_next` | `shm` | 结论 |
| --- | --- | ---: | ---: | --- |
| 多线程 `push_back` | 单条外部锁 `push_back` | 75.9 [64.7,93.0] ns/op | 161.0 [135.2,183.3] ns/op | `shm_next` 稳定更快 |
| 多进程 `push_back` | 单条外部锁 `push_back` | 234.8 [144.4,352.0] ns/op | 200.1 [176.3,245.9] ns/op | 接近但波动较大，不作为 `shm_next` 稳定优势 |
| 多线程 `at` 连续读 | `Synchronized<Vector>::with_lock()` 批量 `at` | 10.1 [7.3,13.6] ns/op | 27.8 [22.7,38.2] ns/op | `shm_next` 稳定更快 |
| 多进程 `at` 连续读 | `Synchronized<Vector>::with_lock()` 批量 `at` | 14.9 [12.1,16.8] ns/op | 30.5 [25.9,35.1] ns/op | `shm_next` 稳定更快 |
| 多线程拷贝读取 | `Synchronized<Vector>::with_lock()` 批量 `operator[] + memcpy` | 5.6 [5.0,7.3] ns/op | `fetch`: 1329.1 [1155.8,1595.2] ns/op | `shm_next` 稳定更快 |
| 多进程拷贝读取 | `Synchronized<Vector>::with_lock()` 批量 `operator[] + memcpy` | 30.9 [27.4,33.5] ns/op | `fetch`: 1391.8 [1249.0,1563.8] ns/op | `shm_next` 稳定更快 |

## 结论

- `push_back`：`shm_next` 在线程场景稳定更快；进程场景两者接近且波动较大，本轮 5 次复测不能证明 `shm_next` 稳定更快。
- 原始单条外部锁 `at`：`shm` 更快，尤其多进程下优势稳定。
- 连续读取 `at`：使用 `Synchronized<Vector>::with_lock()` 批量读取后，`shm_next` 已反超 `shm::at()`。
- 拷贝读取：`shm_next` 的 `operator[] + memcpy` 明显快于 `shm::fetch()`。
- 对性能敏感的 vector 读路径，推荐按批次进入 `with_lock()`；如果调用方已经保证边界安全，可在批量锁内使用 `operator[]`。
