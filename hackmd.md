# 2026q1 Homework2 (stdc)

contributed by < `laneser` >

{%hackmd NrmQUGbRQWemgwPfhzXj6g %}

## 思索〈[分析「快慢指標」](https://hackmd.io/@sysprog/ry8NwAMvT)〉

教材比較兩種找鏈結串列中點的演算法：**快慢指標**（fast-slow pointer）與**單一指標**（two-pass）。兩者存取的節點數相同（$\frac{3}{2}n$），但教材認為快慢指標具有較好的 temporal locality。

- [ ] 設計實驗並在 GNU/Linux 比較文中二個演算法的 cache 行為。
    - 如何在 Linux 上建立長度可控制的鏈結串列（例如 $10^4$、$10^6$、$10^8$）？
    - 如何避免節點在記憶體中連續配置（例如使用 `malloc` + 隨機排列）？
    - 如何使用 [perf stat](https://hackmd.io/@sysprog/linux-perf) 測量： `perf stat -e cache-references,cache-misses,cycles,instructions`
    - 思考 cache miss rate 是否隨 linked list 長度增加而擴大？

### 建立可控長度鏈結串列

使用 `calloc` 或個別 `malloc` 配置節點，以函式參數控制長度。節點結構：

```c
struct list_node {
    int val;
    struct list_node *next;
};
```

每個節點 16 bytes（`int` + padding + `next` 指標）。

### 避免節點在記憶體中連續配置

實驗過程中經歷了三代配置策略，逐步改進：

**第一代：pool + shuffle（有缺陷）**

`calloc(n, 16)` 一次配置所有節點，Fisher-Yates shuffle 串接順序。問題：4 個節點共用一條 64-byte cache line，存取任一節點會「免費」載入同 cache line 的鄰居，低估 cache miss 率。

**第二代：pool + 64-byte stride + shuffle（仍不理想）**

`calloc(n, 64)` 讓每個節點獨佔一條 cache line。解決了 cache line 共用問題，但所有節點仍在同一塊連續記憶體中。即使只有 100 個節點（1.6 KB 實際資料），整個工作集都在 cache 以內——怎麼 shuffle 都不會 miss。真實程式中，100 個節點散落在 heap 各處，可能橫跨數 MB 的位址空間。

**第三代：個別 malloc + spacer（目前使用）**

每個節點個別 `malloc`，中間穿插隨機大小的 spacer 再 `free`：

```c
for (int i = 0; i < n; i++) {
    void *spacer = malloc(64 + (rand() % 4096));
    ptrs[i] = malloc(sizeof(struct list_node));
    free(spacer);
}
```

spacer 把節點推到不同的 cache line 甚至不同的 page 上。`free(spacer)` 後記憶體回到 malloc 的 free list，但節點的位址不變——它們真正散落在 heap 各處。這才能模擬真實的鏈結串列配置狀況。

### 實驗中踩到的坑：`volatile` 的位置

實驗過程中發現 single pointer 的執行時間為 0 ns——原來是 `volatile` 放錯位置：

```c
/* 錯：volatile 修飾「指向的資料」，指標本身的寫入被最佳化掉 */
static volatile struct list_node *sink;

/* 對：volatile 修飾「指標本身」，每次 sink = fn(head) 都不可省略 */
static struct list_node *volatile sink;
```

根據 C99 §6.7.3，`volatile T *p` 表示 `*p` 是 volatile，但 `p` 本身的賦值可被最佳化掉。`T *volatile p` 才能確保對 `p` 的寫入保留。

### 使用 perf stat 測量

`perf stat` 透過硬體效能計數器統計快取行為：

```bash
perf stat -e cache-references,cache-misses,cycles,instructions \
    ./cache_exp 1000000 shuf fast 100
```

注意：`cache-misses` 在不同架構上的語意不同。x86 通常計 LLC（Last Level Cache）miss，ARM Cortex-A53 則計 L1d miss。這在解讀跨平台數據時很重要。

若環境不支援 perf（如 container 被 seccomp 擋掉），可用 Valgrind cachegrind 模擬。

### 實驗結果

實驗在三個平台上執行，節點採分散配置（個別 `malloc` + spacer）。

#### Raspberry Pi（Cortex-A53, L1d = 32 KB, L2 = 512 KB）— perf stat

![Raspberry Pi cache experiment](https://raw.githubusercontent.com/laneser/linux2026_stdc/main/cache_exp_rasberrypi.svg)

Pi 的 `cache-misses` 計數器量測 **L1d miss**。Shuffled 模式的 cache miss 比較：

| 節點數 | fast miss/iter | single miss/iter | miss 比 | 時間比 |
|--------|---------------|-----------------|---------|--------|
| **1K** | **2** | **211** | **105x** | **2.28x** |
| 5K | 6,282 | 6,612 | 1.05x | 1.37x |
| 10K | 13,838 | 14,203 | 1.03x | 1.41x |
| 50K | 74,280 | 74,612 | 1.00x | 1.43x |
| 100K | 171,517 | 171,683 | 1.00x | 1.47x |

**1K 節點的 temporal locality 效應清晰可見**：fast-slow 每次迭代只有 2 個 L1d miss，single 卻有 211 個。差距 209 個 miss × L1→L2 延遲 ~15.7 ns/miss ≈ 3,280 ns，與實測時間差（3,280 ns）完全吻合。

原因：1K 個節點散布在約 2 MB 的位址空間（`malloc` + spacer 效果），超過 L1d 容量（32 KB）。L1d 是 set-associative（通常 8-way, 64 sets），1000 個節點映射到 64 個 set 平均每 set 15.6 個——超過 8 way 的容量，必然有 conflict miss。Fast-slow 的 interleaved 存取讓近期存取的節點留在 L1；single 的 first pass 走完全部節點後，前半段已被後半段逐出。

**5K+ 節點效應消失**：工作集遠超 L1d 和 L2，兩種演算法的 miss 數收斂到幾乎相同（miss 比 ≤ 1.05x）。

#### Intel Celeron J1800（L1d = 48 KB, L2 = 1 MB）— perf stat

![Intel J1800 cache experiment](https://raw.githubusercontent.com/laneser/linux2026_stdc/main/cache_exp_centerm1.svg)

J1800 的 `cache-misses` 計 **LLC (L2) miss**。Shuffled 模式：

| 節點數 | fast miss/iter | single miss/iter | miss 比 | 時間比 |
|--------|---------------|-----------------|---------|--------|
| 1K | 0 | 0 | 1.12x | 1.39x |
| 5K | 0 | 0 | 0.80x | 1.30x |
| 50K | 66,761 | 73,896 | 1.11x | 1.42x |
| 100K | 241,366 | 251,926 | 1.04x | 1.72x |
| 1M | 3,460,053 | 3,471,357 | 1.00x | 1.49x |

1K 節點在 LLC 層看不到差異（L2 = 1MB 容納綽綽有餘），temporal locality 效應發生在 L1d 層但 `cache-misses` 不計 L1d miss。50K 處 single 的 LLC miss 多 11%，但整體差距仍由 loop overhead 主導。

#### DevContainer（Ryzen 7 9800X3D, L1d = 384 KB, L2 = 8 MB, L3 = 96 MB）— cachegrind

![DevContainer cachegrind experiment](https://raw.githubusercontent.com/laneser/linux2026_stdc/main/cache_exp_82a9c2eae741.svg)

cachegrind 模擬的 D1 miss 在 5K 節點時 fast 為 35.6%、single 為 38.4%（差 2.8 pp），與 Pi 的趨勢一致。大 list 因記憶體需求過高（個別 malloc + spacer）而被跳過。

#### 三平台比較

![Cross-platform comparison](https://raw.githubusercontent.com/laneser/linux2026_stdc/main/cache_exp_combined.svg)

> 完整實驗程式碼見 [`cache_exp.c`](https://github.com/laneser/linux2026_stdc/blob/main/cache_exp.c)。
> 讀者可在自己的機器上執行 `make bench plot` 重現實驗並產生 SVG 圖表。

### cache miss rate 是否隨 linked list 長度增加而擴大？

**是**。以 Pi 的 L1d miss 數據為例，fast-slow 每次迭代的 miss 數從 1K 的 2 增長到 100K 的 171,517——miss rate 隨工作集超過各層快取容量而急劇上升。

### 分析：temporal locality 是真的，但只在特定條件下可見

#### 實驗方法對結論影響巨大

同一個問題、同一批演算法，因為**配置策略不同**，得到截然不同的結論：

| 配置方式 | 1K miss 比 (single/fast) | 結論 |
|---------|------------------------|------|
| pool (calloc 一整塊) | 1.00x | 看不到 temporal locality |
| pool + 64-byte stride | 1.00x | 看不到 temporal locality |
| **個別 malloc + spacer** | **105x** | **temporal locality 清晰可見** |

前兩種方式下，小 list 的工作集全部在 cache 以內，不管怎麼 shuffle 都不會 miss。只有讓節點真正散落在 heap 各處，cache 的 set-associative conflict miss 才會出現，temporal locality 的差異才能被量測到。

#### 效應只出現在 L1 邊界附近

Temporal locality 效應在 Pi 的 **1K 節點**（散布在 ~2 MB，超過 L1d 32 KB 但遠小於 L2 512 KB）最明顯：

- fast-slow 每次迭代 2 個 L1d miss，幾乎 100% L1 hit
- single 每次迭代 211 個 L1d miss，14% miss rate
- 時間差（3,280 ns）完全由 L1d miss 差（209 misses × ~15.7 ns L2 latency）解釋

一旦工作集超過 L2（5K+ 節點），兩種演算法的 miss 數就收斂（miss 比 ≤ 1.05x），temporal locality 效應消失。

#### 大 list 的差距來自 loop overhead

5K+ 節點時，shuffled 模式的時間比穩定在 ~1.4x–1.5x，不隨 list 大小變化。這是迴圈結構差異：
- fast-slow：$n/2$ 次迭代，每次 3 個 dereference
- single：$\frac{3n}{2}$ 次迭代，每次 1 個 dereference + counter 操作

### 小結

1. **Temporal locality 是真的**——在 Pi 上 1K 散落節點的 L1d miss 差距達 105 倍，時間差完全可由 cache miss penalty 解釋
2. **但只在特定條件下可見**：節點必須真正散落（非連續配置），且工作集大小在 L1 容量附近
3. **實驗方法至關重要**：連續配置會完全掩蓋 temporal locality 效應。題目要求「避免節點在記憶體中連續配置」正是為此
4. **大 list 的效能差距由 loop overhead 主導**，而非 cache 行為
