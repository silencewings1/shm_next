# shm_next Vector 锁策略性能对比

本文记录 `shm_next::SharedMemoryVector` 在外部 `InterprocessMutex` 与外部 `InterprocessSharedMutex` 两种锁策略下的接口性能。测试目标是验证：对之前低于 `shm::at()` 的 vector 单条读取场景，改用进程共享读写锁是否能追回性能。

## 测试代码

| 锁策略 | 测试文件 |
| --- | --- |
| 外部互斥锁 | `test/container/compare/shm_vector_perf_compare.cpp` |
| 外部读写锁 | `test/container/compare/shm_vector_shared_mutex_perf_compare.cpp` |

## 测试口径

统一参数：

```text
workers=4
ops=50000
record_bytes=64
```

接口覆盖：

| 语义 | 互斥锁测试 | 读写锁测试 |
| --- | --- | --- |
| 单条写入 | `push_back()` + exclusive mutex | `push_back()` + exclusive shared_mutex |
| 原地读取 | `at()` + mutex | `at()` + shared lock |
| 下标读取 | `operator[]` + mutex | `operator[]` + shared lock |
| 拷贝读取 | `operator[] + memcpy` + mutex | `operator[] + memcpy` + shared lock |

说明：

- 读写锁测试仅用于验证锁策略，不改变 `SharedMemoryVector` 容器语义。
- `InterprocessSharedMutex` 基于进程共享 `pthread_rwlock_t`，读路径可以并发，但单次加锁成本高于普通互斥锁。
- 当前测试每条记录读取都单独加锁，代表最细粒度安全访问成本。

## 编译与运行方式

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target \
  test_container_compare_shm_vector_perf_compare \
  test_container_compare_shm_vector_shared_mutex_perf_compare

./build/test/test_container_compare_shm_vector_perf_compare \
  --workers=4 \
  --operations=50000

./build/test/test_container_compare_shm_vector_shared_mutex_perf_compare \
  --workers=4 \
  --operations=50000
```

也可以通过 CTest 运行：

```bash
ctest --test-dir build -R 'test_container_compare_shm_vector.*perf_compare' --output-on-failure
```

## 测试结果

| 场景 | 外部 `InterprocessMutex` | 外部 `InterprocessSharedMutex` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 3.464 ms / 69.3 ns/op | 5.139 ms / 102.8 ns/op | mutex 更快，约 1.5x |
| 多进程 `push_back` | 9.058 ms / 181.2 ns/op | 8.543 ms / 170.9 ns/op | shared_mutex 略快，约 1.1x |
| 多线程 `at` | 2.339 ms / 46.8 ns/op | 3.589 ms / 71.8 ns/op | mutex 更快，约 1.5x |
| 多进程 `at` | 2.716 ms / 54.3 ns/op | 3.128 ms / 62.6 ns/op | mutex 更快，约 1.2x |
| 多线程 `operator[]` | 2.106 ms / 42.1 ns/op | 2.640 ms / 52.8 ns/op | mutex 更快，约 1.3x |
| 多进程 `operator[]` | 2.416 ms / 48.3 ns/op | 4.312 ms / 86.2 ns/op | mutex 更快，约 1.8x |
| 多线程拷贝读取 | `operator[] + memcpy`: 2.607 ms / 52.1 ns/op | `operator[] + memcpy`: 3.500 ms / 70.0 ns/op | mutex 更快，约 1.3x |
| 多进程拷贝读取 | `operator[] + memcpy`: 3.812 ms / 76.2 ns/op | `operator[] + memcpy`: 2.676 ms / 53.5 ns/op | shared_mutex 更快，约 1.4x |

## 结论

- `InterprocessSharedMutex` 不能解决 vector 单条 `at()` 低于 `shm::at()` 的问题；在本轮远程数据中，线程和进程 `at()` 都比外部互斥锁更慢。
- 对 vector 这种极短读操作，单条记录粒度的 `pthread_rwlock` 成本会抵消读并发收益。
- 后续不建议把 vector 默认安全访问从 `InterprocessMutex` 切到 `InterprocessSharedMutex`。
- 更合理的优化方向是减少锁次数：对连续读取场景使用一次加锁读取多条数据，或在调用方已保证边界安全时使用 `operator[]`。
