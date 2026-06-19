# 高頻交易架構改良計畫：非同步持久化與資料庫解耦 (DB Decoupling & Async Persistence)

## 1. 背景與痛點分析
目前系統在處理訂單的 Critical Path 上，包含「同步等待資料庫 (DB) 寫入」。即使是 In-memory DB，其 I/O 與網路開銷仍高達毫秒級別，這會嚴重拖垮 Client Manager 的吞吐量 (Throughput) 並增加尾部延遲 (Tail Latency)。此外，隨著用戶量增加，將所有歷史資料保留在 Matching Engine 的記憶體中將導致 OOM (Out Of Memory) 風險。

## 2. 核心架構變更目標
將 DB 從 Critical Path 中完全移除。DB 將降級為「供查詢用的非同步視圖 (Asynchronous View)」，而系統的**唯一真實來源 (Source of Truth, SoT)** 將轉移到「僅附加的二進位日誌檔 (Append-Only Mmap Log)」。

## 3. 新架構資料流設計
將原本的管線：
`Matching Engine` -> `Response Ring` -> `Client Manager` -> `Send to Client` -> `[Blocking] DB Update`

修改為以 **Mmap Append-Only Log** 為核心的非同步架構：

`Matching Engine` -> `Mmap Append-Only Log` (唯一的 Source of Truth)

下游所有元件皆作為獨立的 Consumer，透過讀取此 Mmap Log 來進行各自的工作：

### A. 極低延遲推播路徑 (Client Manager)
`Mmap Log` -> `Client Manager` -> `WebSocket Send to Client`
*(Client Manager 不斷輪詢 Mmap Log 的尾端。由於資料全在 OS Page Cache 中，讀取速度等同於記憶體，延遲極低，且天然支援斷線重連的歷史回放。)*

### B. 高吞吐持久化路徑 (Async DB Writer)
`Mmap Log` -> `DB Writer Thread` -> `DB`
*(DB Writer 依照自己的節奏，批次讀取 Mmap Log 並寫入關聯式資料庫。無論 DB 延遲多高，都不會對 Matching Engine 產生反壓 Backpressure。)*

## 4. 全域序號 (Global Sequence Number) 與一致性保證
由於 DB 寫入變成非同步，當客戶端收到「成交」的 WebSocket 通知並立刻發送 REST API 查詢帳戶餘額時，DB 可能尚未更新，導致讀到舊資料 (Stale Read)。為了解決這個問題，導入 **Read-Your-Own-Writes (RYOW)** 機制：

1.  **配發序號**：Matching Engine 輸出的每筆 Response 都打上嚴格遞增的 `Global_SeqNo`。
2.  **通知客戶**：Client Manager 傳送給 Client 的 Response 中包含此 `Global_SeqNo`。
3.  **DB 進度標記**：DB Writer 每批次寫入 DB 時，同時在 DB 或共享記憶體中更新 `Last_Applied_Global_SeqNo`。
4.  **智慧等待**：當 Client 呼叫 REST API 時帶上 `seq=1050`，API Server 檢查若 `Last_Applied_Global_SeqNo < 1050`，則內部短暫 Sleep/Poll 幾毫秒，直到 DB 追上進度後才從 DB 撈取資料回傳。這樣客戶永遠不會感覺到資料不同步。

## 5. 記憶體管理 (Working Set)
Matching Engine 的記憶體中只保留**活躍狀態 (Active State)**：
*   目前未成交的掛單 (Open Orders)
*   OrderBook
*   活躍用戶的資產餘額
一旦訂單狀態終結 (Filled, Cancelled, Rejected)，便將事件寫入 Mmap Log，並**立即從 Engine 的記憶體中刪除**。歷史紀錄的查詢責任全權交給下游的 DB、Mmap Log 與 API Server 處理。
