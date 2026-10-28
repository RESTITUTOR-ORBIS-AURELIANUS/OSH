# OSH · AER 页面替换

固定版本：**`aer-v1.0.0`**。算法与代码说明见 [AER 算法介绍与实现](docs/AER.md)。

## 编译、运行

需要支持 C++17 的编译器。在仓库目录执行：

```sh
c++ -O3 -std=c++17 submission.cpp -o pager
./pager data/sort1.txt
```

将 `data/sort1.txt` 换成现场测试文件路径即可。默认运行 AER，输出 **5、10、20、50、100** 个页框的缺页次数、缺页率及平均缺页次数。

输入为用空白分隔的**十六进制页号**，没有文件头，不需要除以页大小；支持 `0x` 前缀。十进制输入加 `--base 10`。

## 常用命令

```sh
./pager --algorithm all data/*.txt       # 比较所有算法，含离线 OPT
./pager --format csv data/sort1.txt      # CSV 输出
./pager --frames 5,20,100 your_trace.txt # 指定页框数
```

macOS/Linux 也可用 `./run.sh your_trace.txt` 自动编译运行（需要 make）。Windows 编译后运行 `pager.exe`。

## 检查与版本

```sh
make test       # 正确性、冻结文件校验、200 项基准回归；需要 Python 3
make benchmark  # 输出到 build/benchmark；绘图可选依赖 matplotlib
```

本版本给定数据平均缺页 **16834.10**，LRU 为 **24438.00**。完整数字在 [results/provided.csv](results/provided.csv)。

[release/manifest.json](release/manifest.json) 固定源码、数据及结果的 SHA-256。维护时编辑 `pager.cpp` 和 `include/`，再用 `make submission` 生成单文件；修改后应创建新版本，保留原标签。
