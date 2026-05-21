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

## 结论

- `shm_next` 在插入和删除上优势明显，尤其删除场景差距最大。
- `shm` 在查找、拷贝读取和已有 key 更新上更快。一个重要原因是 `shm` 的 map 是面向固定 key/value 存储设计的共享内存对象，并且读接口使用 reader lock；`shm_next` 当前测试为了保持线程/进程安全，读操作也用外部互斥锁串行保护。
- 当前比较是“容器可安全并发使用时的端到端接口成本”，不是单纯数据结构裸操作成本。后续如果要进一步定位差异，应增加单线程无锁版本，以及为 `shm_next` 增加进程共享读写锁后再复测读场景。
