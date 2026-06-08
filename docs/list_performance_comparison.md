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

稳定性复测口径：

```text
rounds=5
result=avg[min,max] ns/op
```

判定规则：

- 如果更快一方的最大值仍低于另一方最小值，记为“稳定更快”。
- 如果两边区间重叠但平均值方向一致，记为“平均更快但区间重叠”。

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

## 本轮复测结果

| 场景 | `shm_next` | `shm` | 对比结论 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 9.396 ms / 187.9 ns/op | 30.960 ms / 619.2 ns/op | `shm_next` 更快，约 3.3x |
| 多进程 `push_back` | 7.856 ms / 157.1 ns/op | 46.772 ms / 935.4 ns/op | `shm_next` 更快，约 6.0x |
| 多线程 `front` | 1.965 ms / 39.3 ns/op | 3.836 ms / 76.7 ns/op | `shm_next` 更快，约 2.0x |
| 多进程 `front` | 1.634 ms / 32.7 ns/op | 2.463 ms / 49.3 ns/op | `shm_next` 更快，约 1.5x |
| 多线程 `back` | 2.114 ms / 42.3 ns/op | 2.629 ms / 52.6 ns/op | `shm_next` 更快，约 1.2x |
| 多进程 `back` | 2.292 ms / 45.8 ns/op | 4.089 ms / 81.8 ns/op | `shm_next` 更快，约 1.8x |
| 多线程头部拷贝读取 | `front + memcpy`: 2.086 ms / 41.7 ns/op | `fetch_front`: 2.267 ms / 45.3 ns/op | `shm_next` 略快，约 1.1x |
| 多进程头部拷贝读取 | `front + memcpy`: 2.982 ms / 59.6 ns/op | `fetch_front`: 3.170 ms / 63.4 ns/op | `shm_next` 略快，约 1.1x |
| 多线程 `pop_front` | 3.021 ms / 60.4 ns/op | 30.382 ms / 607.6 ns/op | `shm_next` 更快，约 10.1x |
| 多进程 `pop_front` | 5.850 ms / 117.0 ns/op | 45.097 ms / 901.9 ns/op | `shm_next` 更快，约 7.7x |
| 多线程头部读后消费 | `front + memcpy + pop_front`: 3.227 ms / 64.5 ns/op | `pop_front(buffer)`: 26.922 ms / 538.4 ns/op | `shm_next` 更快，约 8.3x |
| 多进程头部读后消费 | `front + memcpy + pop_front`: 4.679 ms / 93.6 ns/op | `pop_front(buffer)`: 45.670 ms / 913.4 ns/op | `shm_next` 更快，约 9.8x |

## 5 轮稳定性复测结果

| 场景 | `shm_next` avg[min,max] | `shm` avg[min,max] | 稳定性判定 |
| --- | ---: | ---: | --- |
| 多线程 `push_back` | 189.4 [154.8,270.0] ns/op | 607.1 [589.8,651.4] ns/op | `shm_next` 稳定更快 |
| 多进程 `push_back` | 184.4 [166.1,218.3] ns/op | 1033.4 [960.7,1165.8] ns/op | `shm_next` 稳定更快 |
| 多线程 `front` | 40.5 [37.7,44.0] ns/op | 64.7 [50.0,84.2] ns/op | `shm_next` 稳定更快 |
| 多进程 `front` | 44.5 [37.5,50.0] ns/op | 64.1 [51.0,86.1] ns/op | `shm_next` 稳定更快 |
| 多线程 `back` | 40.9 [38.3,44.1] ns/op | 63.2 [57.5,73.5] ns/op | `shm_next` 稳定更快 |
| 多进程 `back` | 48.4 [42.3,54.7] ns/op | 65.8 [51.1,96.5] ns/op | `shm_next` 平均更快但区间重叠 |
| 多线程头部拷贝读取 | `front + memcpy`: 44.9 [40.7,50.7] ns/op | `fetch_front`: 75.6 [61.1,82.0] ns/op | `shm_next` 稳定更快 |
| 多进程头部拷贝读取 | `front + memcpy`: 67.0 [56.5,82.7] ns/op | `fetch_front`: 68.9 [61.0,74.8] ns/op | `shm_next` 平均更快但区间重叠 |
| 多线程 `pop_front` | 81.4 [67.1,102.1] ns/op | 546.6 [518.3,580.9] ns/op | `shm_next` 稳定更快 |
| 多进程 `pop_front` | 133.8 [116.9,159.2] ns/op | 970.9 [945.0,989.7] ns/op | `shm_next` 稳定更快 |
| 多线程头部读后消费 | `front + memcpy + pop_front`: 70.5 [55.6,81.4] ns/op | `pop_front(buffer)`: 604.3 [546.4,684.7] ns/op | `shm_next` 稳定更快 |
| 多进程头部读后消费 | `front + memcpy + pop_front`: 128.3 [112.4,154.2] ns/op | `pop_front(buffer)`: 977.2 [970.2,1001.9] ns/op | `shm_next` 稳定更快 |

## 优化项与实现特点

`SharedMemoryList` 本轮公共接口在原始外部锁口径下大多数场景稳定优于 `shm list`，没有额外引入批量访问或替代容器策略。多进程 `back` 和多进程头部拷贝读取属于平均更快但区间重叠，结论应保守看待。

主要性能来源：

| 技术 / 实现特点 | 作用 |
| --- | --- |
| STL 风格双向链表节点 | `front` / `back` / `pop_front` 路径短，端点访问直接 |
| `SharedMemoryNodePool` 节点缓存 | `pop_front` 后的节点可复用，降低后续节点分配成本 |
| 容器内部无业务锁 | 由调用方外部锁统一保护，容器操作本身保持轻量 |
| `OffsetPtr` 节点链接 | 支持跨进程不同映射地址，同时保持节点结构紧凑 |

`shm list` 更偏共享内存消息队列语义，并带内部头锁 / 尾锁和更多控制逻辑；在本组公共接口下大多数单次接口成本更高。

## 结论

- 在当前这组公共 list 接口下，`shm_next` 大多数场景稳定快于参考 `shm`，尤其是 `push_back`、`pop_front` 和“读后消费”场景。
- 多进程 `back` 和多进程头部拷贝读取只是平均更快但区间重叠，不应作为稳定优势宣传。
- 这和 map 的结果不同，说明 `shm_next` 当前的 list 实现在线性链表节点操作、端点访问路径和对象布局上更轻。
- `shm` 的 list 更偏共享内存消息队列语义，并带有更多内部控制逻辑；即便关闭了预留容量，单次接口成本仍明显更高。
- 本轮仍然测的是“按安全用法加锁后的端到端接口成本”。如果后续要继续细分，可以再增加单生产者单消费者双端并发场景，专门看 `shm` 头锁/尾锁分离是否能在真实流水线负载下追回一部分优势。
