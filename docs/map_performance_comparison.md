# shm_next 与 shm Map 性能对比

本文记录 `shm_next` 与参考项目 `shm` 的 map 接口性能对比。测试覆盖当前阶段可直接对标的单 key 单 value 操作：

- 新 key 插入
- 零拷贝查找
- 拷贝读取
- 已有 key 更新
- 按 key 删除

## 测试代码

| 项目 | 测试文件 |
| --- | --- |
| `shm_next` | `test/container/compare/shm_map_perf_compare.cpp` |
| `shm` | `test/shm/object/MapPerfCompareTest.cpp` |

## 测试口径

统一参数：

```text
workers=4
ops=50000
key=int64_t
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

参考 `shm` map 使用单值模式：

```text
keyLen=8;order=64;keyType=int64;multiData=false
```

锁口径：

| 项目 | 锁策略 |
| --- | --- |
| `shm_next` | 容器内部无锁，每次 map 操作用外部 `InterprocessMutex` 保护 |
| `shm` | 使用容器内部锁；读接口走 reader lock，写接口走 writer lock |

接口对标：

| 语义 | `shm_next` | `shm` |
| --- | --- | --- |
| 新 key 插入 | `try_emplace()` | `insert()` |
| 零拷贝查找 | `find()` 后访问 `it->second` | `find()` 返回 `vector<const char*>` |
| 拷贝读取 | `find() + memcpy` | `fetch()` |
| 已有 key 更新 | `find() + assign` | `update()` |
| 按 key 删除 | `erase(key)` | `erase(key, nullptr)` |

说明：

- `shm` 的 map 支持一 key 多 value，本次只测 `multiData=false`，否则和 `shm_next::SharedMemoryMap<Key, Value>` 语义不一致。
- `shm_next::operator[]` 会在 key 不存在时插入默认 value，不适合作为纯读取接口对比。
- `shm::fetch()` 是拷贝读取，因此不直接和 `shm_next::find()` 或 `at()` 比较，而是和 `find() + memcpy` 比较。
- `shm_next` 当前没有进程共享读写锁，读场景使用外部互斥锁会串行化读操作；`shm` 的读接口内部使用 reader lock，读并发锁模型更有优势。

## 编译与运行方式

### shm_next

远程编译与运行：

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target shm_map_perf_compare

./build/shm_map_perf_compare --workers=4 --operations=50000
```

也可以用 CTest 运行：

```bash
ctest --test-dir build -R shm_map_perf_compare --output-on-failure
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

./testbin/gtshm20 --gtest_filter="MapPerfCompareTest.*" \
  --data_amount=50000 \
  --write_thread_num=4 \
  --read_thread_num=4 \
  --data_length=64
```

说明：

- 当前 `shm` 工程源码使用 C++17 能力，需要以 `-std=c++17` 编译。
- 当前 `shm` 测试构建在远程工程中已开启；如果本地镜像关闭测试入口，可通过 `SHM_BUILD_TESTS=ON` 打开。
- 本地 macOS 镜像不能完整编译 `shm`，因为参考工程使用了 Linux 专用 API，例如 `off64_t`、`lseek64`、`ftruncate64`。性能结论只采用远程 Linux 数据。

## 测试结果

| 场景 | `shm_next` | `shm` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程新 key 插入 | `try_emplace`: 22.368 ms / 447.3 ns/op | `insert`: 136.505 ms / 2730.1 ns/op | `shm_next` 更快，约 6.1x |
| 多进程新 key 插入 | `try_emplace`: 17.028 ms / 340.6 ns/op | `insert`: 111.016 ms / 2220.3 ns/op | `shm_next` 更快，约 6.5x |
| 多线程零拷贝查找 | `find`: 11.792 ms / 235.8 ns/op | `find`: 6.698 ms / 134.0 ns/op | `shm` 更快，约 1.8x |
| 多进程零拷贝查找 | `find`: 17.006 ms / 340.1 ns/op | `find`: 8.209 ms / 164.2 ns/op | `shm` 更快，约 2.1x |
| 多线程拷贝读取 | `find + memcpy`: 12.628 ms / 252.6 ns/op | `fetch`: 8.809 ms / 176.2 ns/op | `shm` 更快，约 1.4x |
| 多进程拷贝读取 | `find + memcpy`: 16.767 ms / 335.3 ns/op | `fetch`: 8.887 ms / 177.7 ns/op | `shm` 更快，约 1.9x |
| 多线程已有 key 更新 | `find + assign`: 17.339 ms / 346.8 ns/op | `update`: 9.912 ms / 198.2 ns/op | `shm` 更快，约 1.7x |
| 多进程已有 key 更新 | `find + assign`: 19.658 ms / 393.2 ns/op | `update`: 11.571 ms / 231.4 ns/op | `shm` 更快，约 1.7x |
| 多线程按 key 删除 | `erase`: 8.135 ms / 162.7 ns/op | `erase`: 98.488 ms / 1969.8 ns/op | `shm_next` 更快，约 12.1x |
| 多进程按 key 删除 | `erase`: 11.710 ms / 234.2 ns/op | `erase`: 98.705 ms / 1974.1 ns/op | `shm_next` 更快，约 8.4x |

## 本轮复测结果

| 场景 | `shm_next` | `shm` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程新 key 插入 | `try_emplace`: 12.229 ms / 244.6 ns/op | `insert`: 99.997 ms / 1999.9 ns/op | `shm_next` 更快，约 8.2x |
| 多进程新 key 插入 | `try_emplace`: 17.254 ms / 345.1 ns/op | `insert`: 106.128 ms / 2122.6 ns/op | `shm_next` 更快，约 6.2x |
| 多线程零拷贝查找 | `find`: 10.112 ms / 202.2 ns/op | `find`: 5.715 ms / 114.3 ns/op | `shm` 更快，约 1.8x |
| 多进程零拷贝查找 | `find`: 14.819 ms / 296.4 ns/op | `find`: 6.815 ms / 136.3 ns/op | `shm` 更快，约 2.2x |
| 多线程拷贝读取 | `find + memcpy`: 14.334 ms / 286.7 ns/op | `fetch`: 7.407 ms / 148.1 ns/op | `shm` 更快，约 1.9x |
| 多进程拷贝读取 | `find + memcpy`: 17.054 ms / 341.1 ns/op | `fetch`: 6.704 ms / 134.1 ns/op | `shm` 更快，约 2.5x |
| 多线程已有 key 更新 | `find + assign`: 10.645 ms / 212.9 ns/op | `update`: 9.391 ms / 187.8 ns/op | `shm` 略快，约 1.1x |
| 多进程已有 key 更新 | `find + assign`: 17.729 ms / 354.6 ns/op | `update`: 11.334 ms / 226.7 ns/op | `shm` 更快，约 1.6x |
| 多线程按 key 删除 | `erase`: 8.528 ms / 170.6 ns/op | `erase`: 85.444 ms / 1708.9 ns/op | `shm_next` 更快，约 10.0x |
| 多进程按 key 删除 | `erase`: 14.961 ms / 299.2 ns/op | `erase`: 93.085 ms / 1861.7 ns/op | `shm_next` 更快，约 6.2x |

## 5 轮稳定性复测结果

| 场景 | `shm_next` avg[min,max] | `shm` avg[min,max] | 稳定性判定 |
| --- | ---: | ---: | --- |
| 多线程新 key 插入 | `try_emplace`: 339.8 [298.6,359.2] ns/op | `insert`: 2215.5 [2126.7,2378.6] ns/op | `shm_next` 稳定更快 |
| 多进程新 key 插入 | `try_emplace`: 387.8 [333.7,420.7] ns/op | `insert`: 2135.6 [2078.1,2168.6] ns/op | `shm_next` 稳定更快 |
| 多线程单条零拷贝查找 | `find`: 261.4 [239.9,302.3] ns/op | `find`: 135.9 [120.9,157.4] ns/op | `shm` 稳定更快 |
| 多进程单条零拷贝查找 | `find`: 321.4 [238.1,372.3] ns/op | `find`: 135.0 [125.7,143.2] ns/op | `shm` 稳定更快 |
| 多线程批量零拷贝查找 | `find`: 100.9 [89.8,110.0] ns/op | `find`: 135.9 [120.9,157.4] ns/op | `shm_next` 稳定更快 |
| 多进程批量零拷贝查找 | `find`: 130.6 [99.0,157.6] ns/op | `find`: 135.0 [125.7,143.2] ns/op | `shm_next` 平均更快但区间重叠 |
| 多线程单条拷贝读取 | `find + memcpy`: 234.6 [198.1,315.0] ns/op | `fetch`: 164.0 [161.1,168.4] ns/op | `shm` 稳定更快 |
| 多进程单条拷贝读取 | `find + memcpy`: 350.6 [325.1,393.3] ns/op | `fetch`: 164.8 [148.1,178.6] ns/op | `shm` 稳定更快 |
| 多线程批量拷贝读取 | `find + memcpy`: 104.3 [90.7,130.6] ns/op | `fetch`: 164.0 [161.1,168.4] ns/op | `shm_next` 稳定更快 |
| 多进程批量拷贝读取 | `find + memcpy`: 135.9 [108.0,172.8] ns/op | `fetch`: 164.8 [148.1,178.6] ns/op | `shm_next` 平均更快但区间重叠 |
| 多线程单条已有 key 更新 | `find + assign`: 253.3 [205.4,290.5] ns/op | `update`: 194.5 [189.7,197.6] ns/op | `shm` 稳定更快 |
| 多进程单条已有 key 更新 | `find + assign`: 387.5 [326.3,455.8] ns/op | `update`: 254.4 [223.5,297.8] ns/op | `shm` 稳定更快 |
| 多线程批量已有 key 更新 | `find + assign`: 109.3 [94.8,120.0] ns/op | `update`: 194.5 [189.7,197.6] ns/op | `shm_next` 稳定更快 |
| 多进程批量已有 key 更新 | `find + assign`: 150.9 [122.7,193.9] ns/op | `update`: 254.4 [223.5,297.8] ns/op | `shm_next` 稳定更快 |
| 多线程按 key 删除 | `erase`: 206.1 [188.4,251.8] ns/op | `erase`: 1908.1 [1865.7,1943.9] ns/op | `shm_next` 稳定更快 |
| 多进程按 key 删除 | `erase`: 298.6 [279.0,327.9] ns/op | `erase`: 1925.9 [1893.0,1984.9] ns/op | `shm_next` 稳定更快 |

## 优化项与推荐用法

`SharedMemoryMap` 是有序红黑树容器。原始单条外部锁测试中，`find`、`find + memcpy`、`find + assign` 低于 `shm map`，主要原因包括：

- 每条操作都单独进入外部 `InterprocessMutex`。
- 红黑树查找需要多次节点跳转和 key 比较。
- `shm map` 是偏固定 key-value 存储的共享内存对象，读接口内部有 reader lock。

已落地的优化技术：

| 优化技术 | 作用 | 适用接口 |
| --- | --- | --- |
| `Synchronized<SharedMemoryMap<K,V>, InterprocessMutex>::with_lock()` | 单次进入临界区后连续执行多次有序 map 操作 | `find()`、`find + memcpy`、`find + assign` |
| 批量访问 | 降低每条 map 操作的外部锁成本 | 连续查找、批量读取、批量更新 |
| 节点缓存 | 复用 erase 后的 map 节点，降低重新插入的分配成本 | 插入 / 删除混合场景 |
| 保留有序语义 | 继续支持有序遍历、`lower_bound`、`upper_bound` | 需要范围查询的业务 |

如果业务只需要单 key 高频读写，不依赖顺序，应优先使用 `SharedMemoryHashMap`。该容器的独立结果见 [HashMap 性能对比](hash_map_performance_comparison.md)。

## 优化后汇总结果

| 场景 | 推荐 `shm_next` 用法 | `shm_next` | `shm` | 结论 |
| --- | --- | ---: | ---: | --- |
| 多线程新 key 插入 | 单条外部锁 `try_emplace` | 339.8 [298.6,359.2] ns/op | `insert`: 2215.5 [2126.7,2378.6] ns/op | `shm_next` 稳定更快 |
| 多进程新 key 插入 | 单条外部锁 `try_emplace` | 387.8 [333.7,420.7] ns/op | `insert`: 2135.6 [2078.1,2168.6] ns/op | `shm_next` 稳定更快 |
| 多线程零拷贝查找 | `Synchronized<Map>::with_lock()` 批量 `find` | 100.9 [89.8,110.0] ns/op | `find`: 135.9 [120.9,157.4] ns/op | `shm_next` 稳定更快 |
| 多进程零拷贝查找 | `Synchronized<Map>::with_lock()` 批量 `find` | 130.6 [99.0,157.6] ns/op | `find`: 135.0 [125.7,143.2] ns/op | `shm_next` 平均更快但区间重叠 |
| 多线程拷贝读取 | `Synchronized<Map>::with_lock()` 批量 `find + memcpy` | 104.3 [90.7,130.6] ns/op | `fetch`: 164.0 [161.1,168.4] ns/op | `shm_next` 稳定更快 |
| 多进程拷贝读取 | `Synchronized<Map>::with_lock()` 批量 `find + memcpy` | 135.9 [108.0,172.8] ns/op | `fetch`: 164.8 [148.1,178.6] ns/op | `shm_next` 平均更快但区间重叠 |
| 多线程已有 key 更新 | `Synchronized<Map>::with_lock()` 批量 `find + assign` | 109.3 [94.8,120.0] ns/op | `update`: 194.5 [189.7,197.6] ns/op | `shm_next` 稳定更快 |
| 多进程已有 key 更新 | `Synchronized<Map>::with_lock()` 批量 `find + assign` | 150.9 [122.7,193.9] ns/op | `update`: 254.4 [223.5,297.8] ns/op | `shm_next` 稳定更快 |
| 多线程按 key 删除 | 单条外部锁 `erase` | 206.1 [188.4,251.8] ns/op | `erase`: 1908.1 [1865.7,1943.9] ns/op | `shm_next` 稳定更快 |
| 多进程按 key 删除 | 单条外部锁 `erase` | 298.6 [279.0,327.9] ns/op | `erase`: 1925.9 [1893.0,1984.9] ns/op | `shm_next` 稳定更快 |

## 结论

- `shm_next` 在插入和删除上优势明显，尤其删除场景差距最大。
- 原始单条外部锁测试中，`shm` 在查找、拷贝读取和已有 key 更新上更快。
- 对连续有序 map 操作，使用 `Synchronized<Map>::with_lock()` 批量访问后，`shm_next` 的查找、拷贝读取和更新整体反超 `shm map`；其中多进程批量查找和多进程批量拷贝读取是平均更快但区间重叠，应按“有优势但存在波动”理解。
- 需要有序遍历或范围查询时继续使用 `SharedMemoryMap`；只需要单 key 高频读写时优先使用 `SharedMemoryHashMap`。
