# 客戶端連線恢復與序號策略 (Client Session Resume & Sequence Strategy)

## 1. 背景與挑戰
在分散式交易系統中，網路不穩定 (斷線、封包遺失) 或客戶端 App 崩潰是常態。客戶端需要一套機制來：
1.  **確保操作的冪等性 (Idempotency)**：如果在斷線瞬間送出了訂單，重連後客戶端不知道該訂單是否被交易所受理，如果盲目重試可能會導致「重複下單」。
2.  **無縫接軌遺漏的訊息 (Catch-up)**：斷線期間如果掛單被觸發成交，重連後客戶端必須能補齊這段期間遺漏的事件。

## 2. 序號設計 (Sequence Strategy)

DB 要維護: 

- Global SeqNum **GSeqNum**, 標示交易所整體事件發生的絕對時間順序。僅用於內部系統追蹤與對齊，**對客戶端隱藏**。
- Client-Wise Incomming (to Server) SeqNum **ISeqNum**: 所有 client 送進來的訊息，包含登入﹑資料請求和訂單
- Client-Wise OutGoing (to Client) SeqNum **OSeqNum**: 所有要送給 client 的訊息
- (client_id, OSeqNum) -> GSeqNum 的 mapping

## 3. 登入

- Server 的 ISeqNum = 5 代表最後一次收到 client 送來的訊息是 5
- Server 的 OSeqNum = 10 代表最後一次送給 client 送來的訊息是 10

**正常情況**:
    - 這時 client 應該要在登入訊息的 MsgSeqNum 欄位放 6，AckSeqNum 放 10，Server 就會認定這是無需 recover 的狀態，直接送 ready frame
    
**可接受異常情況**:
    - client 送了正確的 MsgSeqNum 及過低的 AckSeqNum，Server 必須從 DB 把 client 沒收到的 Executions 重送後，最後送 ready frame (不占 SeqNum)

**不可接受異常情況**:
    - client 送了不正確的 MsgSeqNum 或過高的 AckSeqNum，我們要拒絕連線，並且告訴 client 我們的 expected MsgSeqNum 和 AckSeqNum，讓用戶確認/重新登入。
    - 過高的 MsgSeqNum 表示用戶有訂單送達前出狀況，沒有被 server 接受，用戶有需要可以重送
    
依照此邏輯修改 fbs 及登入流程，就不在登入時送 positions/open orders 等等，讓用戶需要的自己顯式要求