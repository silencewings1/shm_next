# shm_next 优化后性能总览

本文汇总 `shm_next` 在 P0/P1 优化和推荐用法下相对参考项目 `shm` 的性能状态。各容器的详细数据分别整理在独立报告中，本文只做跨报告归纳。

本文性能结论采用 5 轮远程同机复测数据，结果格式为 `avg[min,max] ns/op`。如果两边区间重叠，即使平均值更快，也按“平均更快但存在波动”表述。

## 优化策略

| 问题 | 优化方式 | 对应能力 |
| --- | --- | --- |
| vector 连续读每条记录独立加锁，`at()` 低于 `shm::at()` | 单次锁内批量读取多条记录 | `Synchronized<Vector, InterprocessMutex>::with_lock()` |
| 有序 map 查找/读取/更新每条操作独立加锁，低于 `shm map` | 单次锁内批量执行多次 map 操作 | `Synchronized<Map, InterprocessMutex>::with_lock()` |
| map 高频单 key 读写不需要顺序语义 | 使用无序容器替代有序树 | `SharedMemoryHashMap` |
| list 公共接口已经优于 `shm list` | 保持现有 list 实现和外部锁口径 | `SharedMemoryList` |

## 优化后结果

| 容器 / 场景 | 推荐 `shm_next` 用法 | `shm_next` 结果 | `shm` 对照 | 结论 |
| --- | --- | ---: | ---: | --- |
| vector 多线程 `push_back` | 单条外部锁 `push_back` | 75.9 [64.7,93.0] | 161.0 [135.2,183.3] | `shm_next` 稳定更快 |
| vector 多进程 `push_back` | 单条外部锁 `push_back` | 234.8 [144.4,352.0] | 200.1 [176.3,245.9] | 接近且波动较大，不作为稳定优势 |
| vector 多线程 `at` 连续读 | `Synchronized<Vector>::with_lock()` 批量 `at` | 10.1 [7.3,13.6] | 27.8 [22.7,38.2] | `shm_next` 稳定更快 |
| vector 多进程 `at` 连续读 | `Synchronized<Vector>::with_lock()` 批量 `at` | 14.9 [12.1,16.8] | 30.5 [25.9,35.1] | `shm_next` 稳定更快 |
| vector 多线程拷贝读 | `Synchronized<Vector>::with_lock()` 批量 `operator[] + memcpy` | 5.6 [5.0,7.3] | `fetch`: 1329.1 [1155.8,1595.2] | `shm_next` 稳定更快 |
| vector 多进程拷贝读 | `Synchronized<Vector>::with_lock()` 批量 `operator[] + memcpy` | 30.9 [27.4,33.5] | `fetch`: 1391.8 [1249.0,1563.8] | `shm_next` 稳定更快 |
| 有序 map 多线程插入 | 单条外部锁 `try_emplace` | 339.8 [298.6,359.2] | `insert`: 2215.5 [2126.7,2378.6] | `shm_next` 稳定更快 |
| 有序 map 多进程插入 | 单条外部锁 `try_emplace` | 387.8 [333.7,420.7] | `insert`: 2135.6 [2078.1,2168.6] | `shm_next` 稳定更快 |
| 有序 map 多线程查找 | `Synchronized<Map>::with_lock()` 批量 `find` | 100.9 [89.8,110.0] | `find`: 135.9 [120.9,157.4] | `shm_next` 稳定更快 |
| 有序 map 多进程查找 | `Synchronized<Map>::with_lock()` 批量 `find` | 130.6 [99.0,157.6] | `find`: 135.0 [125.7,143.2] | `shm_next` 平均更快但区间重叠 |
| 有序 map 多线程拷贝读 | `Synchronized<Map>::with_lock()` 批量 `find + memcpy` | 104.3 [90.7,130.6] | `fetch`: 164.0 [161.1,168.4] | `shm_next` 稳定更快 |
| 有序 map 多进程拷贝读 | `Synchronized<Map>::with_lock()` 批量 `find + memcpy` | 135.9 [108.0,172.8] | `fetch`: 164.8 [148.1,178.6] | `shm_next` 平均更快但区间重叠 |
| 有序 map 多线程更新 | `Synchronized<Map>::with_lock()` 批量 `find + assign` | 109.3 [94.8,120.0] | `update`: 194.5 [189.7,197.6] | `shm_next` 稳定更快 |
| 有序 map 多进程更新 | `Synchronized<Map>::with_lock()` 批量 `find + assign` | 150.9 [122.7,193.9] | `update`: 254.4 [223.5,297.8] | `shm_next` 稳定更快 |
| 有序 map 多线程删除 | 单条外部锁 `erase` | 206.1 [188.4,251.8] | `erase`: 1908.1 [1865.7,1943.9] | `shm_next` 稳定更快 |
| 有序 map 多进程删除 | 单条外部锁 `erase` | 298.6 [279.0,327.9] | `erase`: 1925.9 [1893.0,1984.9] | `shm_next` 稳定更快 |
| 无序 key-value 多线程查找 | `SharedMemoryHashMap::find` | 59.4 [47.6,70.6] | `shm map find`: 135.9 [120.9,157.4] | `shm_next` 稳定更快 |
| 无序 key-value 多进程查找 | `SharedMemoryHashMap::find` | 77.7 [61.0,97.5] | `shm map find`: 135.0 [125.7,143.2] | `shm_next` 稳定更快 |
| 无序 key-value 多线程更新 | `SharedMemoryHashMap::find + assign` | 89.7 [74.2,124.0] | `shm map update`: 194.5 [189.7,197.6] | `shm_next` 稳定更快 |
| 无序 key-value 多进程更新 | `SharedMemoryHashMap::find + assign` | 140.5 [118.8,162.8] | `shm map update`: 254.4 [223.5,297.8] | `shm_next` 稳定更快 |
| list 公共接口 | `SharedMemoryList` 单条外部锁 | 40.5-189.4 avg ns/op | 63.2-1033.4 avg ns/op | 大多数场景稳定更快，少数读接口区间重叠 |

## 使用建议

- 连续读取 vector 时，优先使用 `Synchronized<SharedMemoryVector<T>, InterprocessMutex>::with_lock()`，在一次临界区内读取一批元素。
- 有序 map 如果需要范围查询、有序遍历或稳定排序语义，继续使用 `SharedMemoryMap`，但将连续 `find` / `fetch` / `update` 放进同一次 `with_lock()`。
- 如果业务只需要单 key 高频读写，不依赖顺序，优先使用 `SharedMemoryHashMap`。
- `InterprocessSharedMutex` 不适合作为 vector 单条极短读操作的默认优化；远程数据表明减少锁次数比替换锁类型更有效。
- 对平均更快但区间重叠的场景，应继续通过业务负载复测确认，不建议作为稳定性能卖点。

## 相关报告

- [Vector 性能对比](vector_performance_comparison.md)
- [Vector 批量读性能对比](vector_batch_read_performance_comparison.md)
- [Vector 锁策略性能对比](vector_shared_mutex_performance_comparison.md)
- [Map 性能对比](map_performance_comparison.md)
- [Map 批量访问性能对比](map_batch_access_performance_comparison.md)
- [HashMap 性能对比](hash_map_performance_comparison.md)
- [List 性能对比](list_performance_comparison.md)
