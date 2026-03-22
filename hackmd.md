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

#### 建模：以 reuse distance 預測 cache miss

**前提假設：**
- 每個 node 佔一條獨立的 cache line（spacer 保證，前述地址驗證已確認）
- Cache 為 fully associative + LRU 替換（簡化，之後討論 set-associative 的偏差）
- Cache 可容納 $C$ 條 cache line（如 L1d 32KB → $C = 512$）
- **Reuse distance** $d$：兩次存取同一條 cache line 之間，有幾條不同的 cache line 被存取過。若 $d < C$ → hit；$d \geq C$ → miss

**Fast-slow pointer 的存取序列：**

```
iter 1: fast→node[1], fast→node[2], slow→node[1]
iter 2: fast→node[3], fast→node[4], slow→node[2]
iter k: fast→node[2k-1], fast→node[2k], slow→node[k]
```

Slow 在第 $k$ 次迭代存取 node[$k$]，fast 上次存取 node[$k$] 是在第 $\lceil k/2 \rceil$ 次迭代。中間經過約 $k/2$ 次迭代，每次 3 條 cache line（2 fast + 1 slow），大部分是首次存取的不同 node。因此：

$$d_{\text{fast-slow}}(k) \approx \frac{3k}{2}$$

Slow 存取 node[$k$] 時會 hit 的條件：$\frac{3k}{2} < C$，即 $k < \frac{2C}{3}$。

- Fast 存取 $n$ 個 node，全部是 cold miss → $n$ 次 miss
- Slow 存取 $n/2$ 個 node，其中 $\frac{2C}{3}$ 個 hit，其餘 miss

$$M_{\text{fast-slow}} = n + \max\!\left(0,\;\frac{n}{2} - \frac{2C}{3}\right)$$

**Two-pass 的存取序列：**

```
first pass:  node[1], node[2], ..., node[n]
second pass: node[1], node[2], ..., node[n/2]
```

Second pass 存取 node[$k$] 時，上次存取是 first pass 的 node[$k$]。中間經過：
- First pass 剩餘：$n - k$ 條不同的 cache line
- Second pass 已走：$k - 1$ 條（但這些是 first pass 的前段 node，reuse 過）

$$d_{\text{two-pass}}(k) = (n - k) + (k - 1) = n - 1$$

每個 node 的 reuse distance 都是 $n - 1$，跟位置無關：

$$M_{\text{two-pass}} = n + \begin{cases} 0 & \text{if } n < C \\ n/2 & \text{if } n \geq C \end{cases}$$

**兩者的 miss 差距：**

當 $n \gg C$ 時：

$$\Delta M = M_{\text{two-pass}} - M_{\text{fast-slow}} = \frac{2C}{3}$$

關鍵結論：**miss 差距是常數 $\frac{2C}{3}$，由 cache 容量決定，不隨 list 長度增長。**

#### 驗證：Intel Celeron J1800（L2 miss）

J1800 的 `cache-misses` 計 LLC (L2) miss，$C = \frac{1\text{MB}}{64\text{B}} = 16384$，預測差距 $\frac{2 \times 16384}{3} \approx 10923$。

| 節點數 | fast miss | two-pass miss | 實測差距 | 模型預測 | 吻合 |
|--------|-----------|-------------|---------|---------|------|
| 1K | 0 | 0 | 0 | 0（$n < C$） | ✓ |
| 5K | 0 | 0 | 0 | 0（$n < C$） | ✓ |
| 50K | 66,761 | 73,896 | 7,135 | ~10,923 | 偏低 |
| 100K | 241,366 | 251,926 | **10,560** | ~10,923 | ✓ |
| 1M | 3,460,053 | 3,471,357 | **11,304** | ~10,923 | ✓ |

$n < C$ 時兩者都 0 miss；$n \gg C$ 時差距收斂到 ~10,500–11,300，與模型吻合。50K 偏低可能源於 set-associative 的 conflict miss 讓 fully associative 假設不完全成立。

#### 驗證：Raspberry Pi（L1d miss）

Pi 的 `cache-misses` 計 L1d miss，$C = \frac{32\text{KB}}{64\text{B}} = 512$，預測差距 $\frac{2 \times 512}{3} \approx 341$。

**模型適用範圍內（$n \gg C$ 且無記憶體壓力）：**

| 節點數 | fast miss | two-pass miss | 實測差距 | 模型預測 | 吻合 |
|--------|-----------|-------------|---------|---------|------|
| 1K | 2 | 211 | 209 | ~341 | 偏低 |
| 5K | 6,282 | 6,612 | **330** | ~341 | ✓ |
| 10K | 13,838 | 14,203 | **365** | ~341 | ✓ |
| 50K | 74,280 | 74,612 | **332** | ~341 | ✓ |
| 100K | 171,517 | 171,683 | 166 | ~341 | 偏低 |

5K–50K 的差距穩定在 330–365，與模型高度吻合。

**探索模型邊界——小 $n$（$n < C$）：**

模型預測 $n < C = 512$ 時兩者都 0 miss。實測：

| 節點數 | fast miss/iter | two-pass miss/iter | 差距 |
|--------|---------------|-------------------|------|
| 100 | 5.2 | 5.3 | ~0 |
| 256 | 44.6 | 38.8 | ~0 |
| 512 | 221.5 | 244.3 | 22.8 |

兩者差距趨近 0（符合模型），但 miss 數不是 0。原因：模型假設 fully associative cache，但 Cortex-A53 的 L1d 是 **4-way set-associative**（128 set）。即使只有 100 個 node，spacer 讓 node 散布在 ~MB 級位址空間，多個 node 的位址 bit[6:12] 相同就會映射到同一個 set，超過 4 way 容量時產生 **conflict miss**。

**探索模型邊界——大 $n$（記憶體壓力）：**

| 節點數 | fast miss/iter | two-pass miss/iter | 實測差距 | 模型預測 | 吻合 |
|--------|---------------|-------------------|---------|---------|------|
| 200K | 888,397 | 913,009 | 24,612 | ~341 | ❌ |
| 500K | 2,999,844 | 2,798,664 | -201,180 | ~341 | ❌ |

200K 以上完全偏離模型。500K 甚至出現 fast miss 比 two-pass 多的反轉。觀察執行時間：500K 節點時 sys time 高達 121 秒（user time 僅 3.5 秒），顯示大量時間花在核心態。每個 node 個別 `malloc` 加上隨機大小 spacer，500K 個 node 的虛擬位址空間可能達數 GB，超過 Pi 的實體記憶體（1 GB），觸發大量 **page fault 甚至 swap**。此時 perf 的 `cache-misses` 計數器已無法反映純粹的 cache 行為。

**模型適用邊界：**

$$C \ll n \ll \frac{\text{物理記憶體}}{\text{平均 spacer 大小}}$$

左界確保工作集超過 cache（差距才會穩定），右界確保不觸發 page fault / swap。在 Pi 上這個範圍約為 $5\text{K} \leq n \leq 50\text{K}$。

#### 回答

Cache miss 數確實隨 linked list 長度增加而增長，但兩種演算法之間的 **miss 差距是常數**（$\approx \frac{2C}{3}$），由 cache 容量決定，不隨 list 長度增長。此結論在 $C \ll n \ll \frac{\text{物理記憶體}}{\text{spacer 大小}}$ 的範圍內成立：過小的 $n$ 因 set-associative conflict 讓模型偏差增大；過大的 $n$ 因記憶體壓力（page fault / swap）讓快取計數器失去意義。
