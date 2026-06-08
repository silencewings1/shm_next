# shm_next Map 批量访问性能对比

本文记录 `shm_next::SharedMemoryMap` 在“单次加锁执行多次有序 map 操作”场景下的性能。测试目标是验证：此前 `find`、`find + memcpy`、`find + assign` 低于 `shm map` 的问题，是否可以通过 `Synchronized<Map, InterprocessMutex>` 减少锁次数解决，同时保留有序 map 语义。

## 测试代码

| 项目 | 测试文件 |
| --- | --- |
| `shm_next` 单条外部锁访问 | `test/container/compare/shm_map_perf_compare.cpp` |
| `shm_next` 批量外部锁访问 | `test/container/compare/shm_map_batch_access_perf_compare.cpp` |
| `shm` 单条内部锁访问 | `test/shm/object/MapPerfCompareTest.cpp` |

## 测试口径

统一参数：

```text
workers=4
ops=50000
key=int64_t
record_bytes=64
batch_size=256
```

稳定性复测口径：

```text
rounds=5
result=avg[min,max] ns/op
```

接口覆盖：

| 语义 | `shm_next` 批量访问 | 对照项 |
| --- | --- | --- |
| 零拷贝查找 | `Synchronized<Map, InterprocessMutex>::with_lock()` 内连续调用 `find()` | `shm::find()` |
| 拷贝读取 | `Synchronized<Map, InterprocessMutex>::with_lock()` 内连续 `find() + memcpy` | `shm::fetch()` |
| 已有 key 更新 | `Synchronized<Map, InterprocessMutex>::with_lock()` 内连续 `find() + assign` | `shm::update()` |

说明：

- 该测试不改变 `SharedMemoryMap` API，而是通过 `interprocess/sync/synchronized.h` 提供的同步包装器验证更合理的调用方式。
- `SharedMemoryMap` 仍然保留有序遍历、`lower_bound`、`upper_bound` 等有序 map 语义。
- 如果业务不需要有序语义，`SharedMemoryHashMap` 仍是更适合单 key 高频读写的容器。

## 编译与运行方式

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target test_container_compare_shm_map_batch_access_perf_compare

./build/test/test_container_compare_shm_map_batch_access_perf_compare \
  --workers=4 \
  --operations=50000 \
  --batch-size=256
```

也可以通过 CTest 运行：

```bash
ctest --test-dir build -R test_container_compare_shm_map_batch_access_perf_compare --output-on-failure
```

## 测试结果

| 场景 | `shm_next` 单条外部锁 avg[min,max] | `shm_next` 批量外部锁 avg[min,max] | `shm` 单条内部锁 avg[min,max] | 稳定性判定 |
| --- | ---: | ---: | ---: | --- |
| 多线程零拷贝查找 | `find`: 261.4 [239.9,302.3] ns/op | `find`: 100.9 [89.8,110.0] ns/op | `find`: 135.9 [120.9,157.4] ns/op | 批量访问后 `shm_next` 稳定更快 |
| 多进程零拷贝查找 | `find`: 321.4 [238.1,372.3] ns/op | `find`: 130.6 [99.0,157.6] ns/op | `find`: 135.0 [125.7,143.2] ns/op | 批量访问后 `shm_next` 平均更快但区间重叠 |
| 多线程拷贝读取 | `find + memcpy`: 234.6 [198.1,315.0] ns/op | `find + memcpy`: 104.3 [90.7,130.6] ns/op | `fetch`: 164.0 [161.1,168.4] ns/op | 批量访问后 `shm_next` 稳定更快 |
| 多进程拷贝读取 | `find + memcpy`: 350.6 [325.1,393.3] ns/op | `find + memcpy`: 135.9 [108.0,172.8] ns/op | `fetch`: 164.8 [148.1,178.6] ns/op | 批量访问后 `shm_next` 平均更快但区间重叠 |
| 多线程已有 key 更新 | `find + assign`: 253.3 [205.4,290.5] ns/op | `find + assign`: 109.3 [94.8,120.0] ns/op | `update`: 194.5 [189.7,197.6] ns/op | 批量访问后 `shm_next` 稳定更快 |
| 多进程已有 key 更新 | `find + assign`: 387.5 [326.3,455.8] ns/op | `find + assign`: 150.9 [122.7,193.9] ns/op | `update`: 254.4 [223.5,297.8] ns/op | 批量访问后 `shm_next` 稳定更快 |

## 结论

- `SharedMemoryMap` 此前在查找、拷贝读取、更新上低于 `shm map`，主要原因之一是每条操作独立进入外部互斥锁。
- 在保留有序 map 语义的前提下，通过 `Synchronized<Map, InterprocessMutex>::with_lock()` 批量访问可以让这些接口整体反超 `shm map`；其中多进程零拷贝查找和多进程拷贝读取是平均更快但区间重叠。
- 业务如果需要有序遍历或范围查询，推荐继续使用 `SharedMemoryMap`，但把一批连续读写操作放进同一次 `with_lock()`。
- 业务如果只需要单 key 高频读写，不依赖顺序，推荐使用 `SharedMemoryHashMap`；5 轮远程复测中它的各对标接口均稳定快于 `shm map`。
