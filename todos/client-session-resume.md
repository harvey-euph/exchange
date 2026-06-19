# 客戶端連線恢復與序號策略 (Client Session Resume & Sequence Strategy)

## 1. 背景與挑戰
在分散式交易系統中，網路不穩定 (斷線、封包遺失) 或客戶端 App 崩潰是常態。當非同步架構 (Async DB Update) 引入後，客戶端需要一套機制來：
1.  **確保操作的冪等性 (Idempotency)**：如果在斷線瞬間送出了訂單，重連後客戶端不知道該訂單是否被交易所受理，如果盲目重試可能會導致「重複下單」。
2.  **無縫接軌遺漏的訊息 (Catch-up)**：斷線期間如果掛單被觸發成交，重連後客戶端必須能補齊這段期間遺漏的事件。

## 2. 序號設計 (Sequence Strategy)

### A. 全域序號 (Global Sequence Number)
*   **維護者**：Matching Engine (伺服器端)。
*   **用途**：標示交易所整體事件發生的絕對時間順序。僅用於內部系統追蹤與對齊，**對客戶端隱藏**。

### B. 客戶端專屬序號 (Client Sequence Number, `Client_Seq`)
*   **維護者**：Client App (客戶端)。
*   **用途**：防呆、去重 (Deduplication)、防止重放攻擊 (Replay Attack) 以及**訊息關聯 (Message Correlation)**。
*   **行為**：每個 Client 登入後維護一個遞增的 `Client_Seq`。**所有**由客戶端發出的請求 (包含 Login, Order, Cancel 等) 都必須帶上此序號。同時，伺服器端推送的**所有**相關回覆與被動事件 (例如 Order Response, Fill Event)，也都必須「Echo (回傳)」最初產生該動作的 `Client_Seq`。這樣 Client 就能完美且輕鬆地將非同步的推播事件對應回本地端的 Request。

### C. 用戶事件序號 (User Event Sequence, `User_Event_Seq`)
*   **維護者**：伺服器端 (Recovery Service / Client Manager)。
*   **用途**：確保客戶端能無縫接軌斷線期間的所有主動與被動事件 (如被動成交)。
*   **行為**：伺服器針對每個活躍帳戶維護一個獨立遞增的事件序號。推送給該帳戶的每筆事件都帶有此序號，客戶端將以此序號請求資料補齊。

## 3. 冪等性實作細節
Matching Engine 必須針對每個活躍的 `client_id` 維護一個 `Last_Seen_Client_Seq` 以及對應的處理結果 (例如 Order ID 或 Reject Code 的 Cache)。

*   **正常情況**：收到 `Client_Seq = 5`，且 `Last_Seen_Client_Seq = 4`。Engine 正常處理，更新 `Last_Seen = 5` 並暫存結果。
*   **重複請求**：收到 `Client_Seq = 5`，但 `Last_Seen_Client_Seq >= 5`。Engine 判斷為重複請求，**不再次撮合**，而是直接從暫存區撈出先前的結果，再次回傳給 Client。

## 4. 連線恢復 (Session Resume) 流程
當客戶端的 WebSocket 斷線並重新連線時，不應該只是單純的「重新訂閱」，而是要經過一個 **Catch-up (追趕)** 階段。
為避免查表與回放影響即時交易效能，此流程中的「專責 Recovery Service」可以直接與 **DB Writer (非同步持久化模組)** 共用或緊密結合。因為 DB Writer 本身就負責處理全域事件並落盤，由它來統籌管理 Sequence 與 State 能確保最佳的狀態同步，不占用 Critical Path 資源。

### Step 1: Login & Handshake
Client 發送 `LoginRequest` 給 Recovery Service (由 DB Writer 兼任或協同)，並在 Request 中聲明此登入請求的預期序號為 `Last_User_Event_Seq + 1`。
*(例如：Client 斷線前收到的最後一個推播事件是 150，則認為這次的 LoginRequest 應該發生在 151 的位置)*

### Step 2: Catch-up Phase (追趕階段)
1.  Recovery Service 收到登入請求後，比對資料庫或 DB Writer 當下該帳戶已持久化的最新事件序號。
2.  **無遺漏**：如果最新序號剛好是 150，代表 Client 沒有漏接訊息。伺服器接受登入，並將登入成功的回覆標記為 151 發給 Client。
3.  **有遺漏**：如果發現歷史紀錄中已經存在 151、152 等事件（斷線期間的被動成交），就會知道 Client「搞錯了（漏接資料）」。此時會直接從 DB 或 DB Writer 的近期快取中，撈出 `>= 151` 的所有歷史訊息補發給 Client，再完成登入交接。

### Step 3: Live Streaming Phase (即時串流階段)
當歷史訊息補發完畢，Recovery Service 會將該連線移交 (Handover) 或無縫切換給負責即時訂閱的 Client Manager。Client Manager 接著把最新的事件即時轉發給 Client。

## 5. 實作效益
搭配 `architecture-db-decoupling.md` 的規劃，這套機制能確保：
*   後端架構可以非同步瘋狂狂奔 (極限低延遲)。
*   前端無論怎麼斷線、重試，都不會發生幽靈訂單、重複下單、或是成交狀態對不起來的客訴問題。
*   資料庫的效能不再成為綁架系統擴充性的瓶頸。
