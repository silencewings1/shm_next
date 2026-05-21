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

## 结论

- `push_back`：`shm_next` 在线程场景明显更快；进程场景两者基本持平。
- 原地读取 `at`：`shm` 更快，尤其多进程下优势稳定。
- 拷贝读取：`shm_next` 的 `operator[] + memcpy` 明显快于 `shm::fetch()`。
- 后续如果继续比较 map/list 或更复杂负载，应继续保持同一台远程机器、同一数据量、同一锁粒度，并明确区分原地读取与拷贝读取语义。
