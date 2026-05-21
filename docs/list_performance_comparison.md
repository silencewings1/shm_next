# shm_next 与 shm List 性能对比

本文记录 `shm_next` 与参考项目 `shm` 的 list 接口性能对比。由于两边 list 抽象层次不同，本轮只比较双方都具备的公共接口语义：

- 尾部写入
- 头部零拷贝读取
- 尾部零拷贝读取
- 头部拷贝读取
- 头部删除
- 头部读后消费

## 测试代码

| 项目 | 测试文件 |
| --- | --- |
| `shm_next` | `test/container/compare/shm_list_perf_compare.cpp` |
| `shm` | `test/shm/object/ListPerfCompareTest.cpp` |

## 测试口径

统一参数：

```text
workers=4
ops=50000
record_bytes=64
```

数据模型：

- `shm_next`：`SharedMemoryList<Record>`
- `shm`：`CShmList`，每条记录固定写入 64B 二进制 `Record`

参考 `shm` list 使用不预留缓存节点的配置，尽量避免把它的预分配优势算进接口比较：

```text
data_length=64;capacity=0
```

锁口径：

| 项目 | 锁策略 |
| --- | --- |
| `shm_next` | 容器内部无锁，每次 list 操作用外部 `InterprocessMutex` 保护 |
| `shm` | 使用容器内部锁，`push_back/back` 走尾锁，`front/fetch_front/pop_front` 走头锁 |

接口对标：

| 语义 | `shm_next` | `shm` |
| --- | --- | --- |
| 尾部写入 | `push_back()` | `push_back()` |
| 头部零拷贝读取 | `front()` | `front()` |
| 尾部零拷贝读取 | `back()` | `back()` |
| 头部拷贝读取 | `front() + memcpy` | `fetch_front()` |
| 头部删除 | `pop_front()` | `pop_front(nullptr, 0)` |
| 头部读后消费 | `front() + memcpy + pop_front()` | `pop_front(buffer, len)` |

说明：

- `shm_next` 是 STL 风格双向链表，支持 `push_front/pop_back/insert/erase/splice/sort/merge/unique` 等高级接口。
- `shm` 的公开 list 接口更接近共享内存队列，只暴露 `push_back/front/back/fetch_front/pop_front`。
- 所以本轮不比较 `push_front`、`pop_back`、`splice`、`sort`、`merge` 之类只在 `shm_next` 中存在的接口。
- `front/back` 场景测的是接口与锁路径成本，因此会反复读取队头或队尾同一条记录，而不是遍历整个 list。

## 编译与运行方式

### shm_next

远程编译与运行：

```bash
cd <remote-shm_next-workspace>

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4 --target test_container_compare_shm_list_perf_compare

./build/test/test_container_compare_shm_list_perf_compare \
  --workers=4 \
  --operations=50000
```

也可以通过 CTest 运行：

```bash
ctest --test-dir build -R test_container_compare_shm_list_perf_compare --output-on-failure
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

./testbin/gtshm20 --gtest_filter="ListPerfCompareTest.*" \
  --data_amount=50000 \
  --write_thread_num=4 \
  --read_thread_num=4 \
  --data_length=64
```

说明：

- 当前 `shm` 工程源码使用 Linux 专用 API，性能结论只采用远程 Linux 数据。
- 当前 `shm` 的 list 接口使用内部头锁/尾锁分离，这会天然改善典型 producer/consumer 模式下的并发性能。

## 测试结果

| 场景 | `shm_next` | `shm` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 9.310 ms / 186.2 ns/op | 29.353 ms / 587.1 ns/op | `shm_next` 更快，约 3.2x |
| 多进程 `push_back` | 9.238 ms / 184.8 ns/op | 47.567 ms / 951.3 ns/op | `shm_next` 更快，约 5.1x |
| 多线程 `front` | 1.785 ms / 35.7 ns/op | 3.653 ms / 73.1 ns/op | `shm_next` 更快，约 2.0x |
| 多进程 `front` | 1.896 ms / 37.9 ns/op | 3.594 ms / 71.9 ns/op | `shm_next` 更快，约 1.9x |
| 多线程 `back` | 2.329 ms / 46.6 ns/op | 3.064 ms / 61.3 ns/op | `shm_next` 更快，约 1.3x |
| 多进程 `back` | 1.979 ms / 39.6 ns/op | 3.363 ms / 67.3 ns/op | `shm_next` 更快，约 1.7x |
| 多线程头部拷贝读取 | `front + memcpy`: 1.811 ms / 36.2 ns/op | `fetch_front`: 3.333 ms / 66.7 ns/op | `shm_next` 更快，约 1.8x |
| 多进程头部拷贝读取 | `front + memcpy`: 2.822 ms / 56.4 ns/op | `fetch_front`: 4.188 ms / 83.8 ns/op | `shm_next` 更快，约 1.5x |
| 多线程 `pop_front` | 11.963 ms / 239.3 ns/op | 27.847 ms / 556.9 ns/op | `shm_next` 更快，约 2.3x |
| 多进程 `pop_front` | 8.605 ms / 172.1 ns/op | 45.263 ms / 905.3 ns/op | `shm_next` 更快，约 5.3x |
| 多线程头部读后消费 | `front + memcpy + pop_front`: 11.731 ms / 234.6 ns/op | `pop_front(buffer)`: 29.146 ms / 582.9 ns/op | `shm_next` 更快，约 2.5x |
| 多进程头部读后消费 | `front + memcpy + pop_front`: 10.732 ms / 214.6 ns/op | `pop_front(buffer)`: 48.047 ms / 960.9 ns/op | `shm_next` 更快，约 4.5x |

## 结论

- 在当前这组公共 list 接口下，`shm_next` 全面快于参考 `shm`，尤其是 `push_back`、`pop_front` 和“读后消费”场景。
- 这和 map 的结果不同，说明 `shm_next` 当前的 list 实现在线性链表节点操作、端点访问路径和对象布局上更轻。
- `shm` 的 list 更偏共享内存消息队列语义，并带有更多内部控制逻辑；即便关闭了预留容量，单次接口成本仍明显更高。
- 本轮仍然测的是“按安全用法加锁后的端到端接口成本”。如果后续要继续细分，可以再增加单生产者单消费者双端并发场景，专门看 `shm` 头锁/尾锁分离是否能在真实流水线负载下追回一部分优势。
