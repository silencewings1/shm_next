# shm_next HashMap 与 shm Map 性能对比

本文记录 `shm_next::SharedMemoryHashMap` 与参考项目 `shm` map 的接口性能对比。该测试用于验证：当业务不要求 key 有序遍历时，使用无序容器能否改善 `SharedMemoryMap` 在查找、拷贝读取和更新路径上低于 `shm` 的问题。

## 测试代码

| 项目 | 测试文件 |
| --- | --- |
| `shm_next` | `test/container/compare/shm_hash_map_perf_compare.cpp` |
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

锁口径：

| 项目 | 锁策略 |
| --- | --- |
| `shm_next` | 容器内部无锁，每次 hash_map 操作用外部 `InterprocessMutex` 保护 |
| `shm` | 使用容器内部锁；读接口走 reader lock，写接口走 writer lock |

接口对标：

| 语义 | `shm_next` | `shm` |
| --- | --- | --- |
| 新 key 插入 | `SharedMemoryHashMap::try_emplace()` | `insert()` |
| 零拷贝查找 | `find()` 后访问 `it->second` | `find()` 返回 `vector<const char*>` |
| 拷贝读取 | `find() + memcpy` | `fetch()` |
| 已有 key 更新 | `find() + assign` | `update()` |
| 按 key 删除 | `erase(key)` | `erase(key, nullptr)` |

说明：

- 本对比不替代 `SharedMemoryMap` 与 `shm map` 的有序 map 对比；它用于评估无序 key-value 场景下更合适的数据结构。
- `SharedMemoryHashMap` 不保证 key 顺序。如果业务依赖有序遍历、`lower_bound`、`upper_bound` 或范围查询，仍应使用 `SharedMemoryMap`。
- 当前测试仍按安全用法对 `shm_next` 外部加互斥锁，因此结果是端到端接口成本，不是裸容器单线程成本。

## 编译与运行方式

### shm_next

远程编译与运行：

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target test_container_compare_shm_hash_map_perf_compare

./build/test/test_container_compare_shm_hash_map_perf_compare \
  --workers=4 \
  --operations=50000
```

也可以通过 CTest 运行：

```bash
ctest --test-dir build -R test_container_compare_shm_hash_map_perf_compare --output-on-failure
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

## 5 轮稳定性复测结果

| 场景 | `shm_next::SharedMemoryHashMap` avg[min,max] | `shm map` avg[min,max] | 稳定性判定 |
| --- | ---: | ---: | --- |
| 多线程新 key 插入 | `try_emplace`: 268.6 [187.1,449.2] ns/op | `insert`: 2215.5 [2126.7,2378.6] ns/op | `shm_next` 稳定更快 |
| 多进程新 key 插入 | `try_emplace`: 193.5 [163.5,222.7] ns/op | `insert`: 2135.6 [2078.1,2168.6] ns/op | `shm_next` 稳定更快 |
| 多线程零拷贝查找 | `find`: 59.4 [47.6,70.6] ns/op | `find`: 135.9 [120.9,157.4] ns/op | `shm_next` 稳定更快 |
| 多进程零拷贝查找 | `find`: 77.7 [61.0,97.5] ns/op | `find`: 135.0 [125.7,143.2] ns/op | `shm_next` 稳定更快 |
| 多线程拷贝读取 | `find + memcpy`: 54.6 [48.2,65.2] ns/op | `fetch`: 164.0 [161.1,168.4] ns/op | `shm_next` 稳定更快 |
| 多进程拷贝读取 | `find + memcpy`: 88.9 [69.2,107.7] ns/op | `fetch`: 164.8 [148.1,178.6] ns/op | `shm_next` 稳定更快 |
| 多线程已有 key 更新 | `find + assign`: 89.7 [74.2,124.0] ns/op | `update`: 194.5 [189.7,197.6] ns/op | `shm_next` 稳定更快 |
| 多进程已有 key 更新 | `find + assign`: 140.5 [118.8,162.8] ns/op | `update`: 254.4 [223.5,297.8] ns/op | `shm_next` 稳定更快 |
| 多线程按 key 删除 | `erase`: 71.1 [52.5,83.8] ns/op | `erase`: 1908.1 [1865.7,1943.9] ns/op | `shm_next` 稳定更快 |
| 多进程按 key 删除 | `erase`: 131.2 [95.0,162.8] ns/op | `erase`: 1925.9 [1893.0,1984.9] ns/op | `shm_next` 稳定更快 |

## 优化技术说明

`SharedMemoryHashMap` 相对有序 `SharedMemoryMap` 的主要优化点：

| 优化技术 | 作用 |
| --- | --- |
| 无序 hash table | 将单 key 查找从红黑树路径改为 bucket 查找，降低节点跳转和比较次数 |
| bucket reserve / rehash | 测试前按目标数据量预留 bucket，避免读写阶段频繁 rehash |
| 节点缓存 | erase 后缓存节点，后续 insert 可复用节点，降低 allocator 调用 |
| 外部互斥锁保持一致 | 与 `SharedMemoryMap` 一样使用外部 `InterprocessMutex`，保证对比不靠弱化同步获得优势 |

推荐规则：

- 需要有序遍历、范围查询、`lower_bound`、`upper_bound`：使用 `SharedMemoryMap`。
- 只需要单 key 高频插入、查找、更新、删除：使用 `SharedMemoryHashMap`。

## 结论

- 在不要求 key 有序的场景下，`SharedMemoryHashMap` 各对标接口均稳定快于参考 `shm map`。
- 这说明此前 `SharedMemoryMap` 在 `find`、`fetch`、`update` 上低于 `shm map`，主要不是共享内存 allocator 或外部互斥锁不可接受，而是红黑树结构不适合固定 key-value 高频点查。
- 对性能敏感、以单 key 读写为主的业务，建议优先使用 `SharedMemoryHashMap`；只有需要有序遍历或范围查询时再使用 `SharedMemoryMap`。
