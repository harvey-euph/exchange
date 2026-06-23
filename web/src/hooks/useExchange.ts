import { useState, useRef, useCallback, useEffect } from 'react';
import * as flatbuffers from 'flatbuffers';
import { L2Update } from '../fbs/exchange/l2-update';
import { ClientResponse } from '../fbs/exchange/client-response';
import { ClientResponseData } from '../fbs/exchange/client-response-data';
import { Side } from '../fbs/exchange/side';
import { ExecType } from '../fbs/exchange/exec-type';
import { OrderResponse } from '../fbs/exchange/order-response';
import { PositionResponse } from '../fbs/exchange/position-response';
import { OrderRequest } from '../fbs/exchange/order-request';
import { OrderAction } from '../fbs/exchange/order-action';
import { OrderType } from '../fbs/exchange/order-type';
import { ClientRequest } from '../fbs/exchange/client-request';
import { ClientRequestData as ClientReqData } from '../fbs/exchange/client-request-data';
import { AdminRequest } from '../fbs/exchange/admin-request';
import { AdminAction } from '../fbs/exchange/admin-action';
import { AdminResponse } from '../fbs/exchange/admin-response';
import { AdminResponseType } from '../fbs/exchange/admin-response-type';
import { OpenOrderRequest } from '../fbs/exchange/open-order-request';
import { PositionRequest } from '../fbs/exchange/position-request';
import { RejectCode } from '../fbs/exchange/reject-code';
import type { OrderData, ConnectedState, SymbolPosition, SymbolInfoData } from '../types';
import { formatPrice, parsePrice } from '../types';
import { MarketDataRequest } from '../fbs/exchange/market-data-request';
import { SymbolInfo } from '../fbs/exchange/symbol-info';
import { MDType } from '../fbs/exchange/mdtype';
import { SubType } from '../fbs/exchange/sub-type';
import { MarketDataUpdate } from '../fbs/exchange/market-data-update';
import { MarketDataUpdateData } from '../fbs/exchange/market-data-update-data';

/**
 * Simple DJB2-like hash for mapping alphanumeric strings to uint32
 */
function hashClientId(id: string): number {
  let hash = 5381;
  for (let i = 0; i < id.length; i++) {
    hash = (hash * 33) ^ id.charCodeAt(i);
  }
  return (hash & 0x7FFFFFFF); // Convert to positive signed 32-bit integer
}

function validatePrice(pVal: bigint, symbolInfo: SymbolInfoData | null): string | null {
  if (!symbolInfo) return null;
  const priceMin = BigInt(symbolInfo.priceMin);
  const priceMax = BigInt(symbolInfo.priceMax);
  const priceMinStep = BigInt(symbolInfo.priceMinStep);
  if (pVal < priceMin || pVal > priceMax) {
    return `Price ${pVal} is out of bounds [${priceMin}, ${priceMax}]`;
  }
  if (priceMinStep > 0n && pVal % priceMinStep !== 0n) {
    return `Price ${pVal} is not a multiple of step size ${priceMinStep}`;
  }
  return null;
}

const REJECT_MESSAGES: Record<number, string> = {
  [RejectCode.PriceInvalid]: 'Invalid Price',
  [RejectCode.OrderNotFound]: 'Order Not Found',
  [RejectCode.InvalidAction]: 'Invalid Action',
  [RejectCode.InvalidModify]: 'Invalid Modify Request',
  [RejectCode.DuplicateOrderID]: 'Duplicate Order ID',
  [RejectCode.Unknown]: 'Unknown Error',
};

export function useExchange(activeSymbolId: number, onNotification?: (type: 'acked' | 'rejected' | 'info', title: string, content: string) => void) {
  const [connected, setConnected] = useState<ConnectedState>({ mgmt: false, mgmtReady: false, l2: false });
  const [bids, setBids] = useState<Map<bigint, bigint>>(new Map());
  const [asks, setAsks] = useState<Map<bigint, bigint>>(new Map());
  const [prices, setPrices] = useState<Map<number, bigint>>(new Map());
  const [openOrders, setOpenOrders] = useState<Map<string, OrderData>>(new Map());
  const [positions, setPositions] = useState<Map<number, SymbolPosition>>(new Map());
  const [cash, setCash] = useState<bigint>(0n);
  const [subscribedSymbols, setSubscribedSymbols] = useState<Set<number>>(new Set());
  const [mgmtLogs, setMgmtLogs] = useState<string[]>([]);
  const [symbolInfo, setSymbolInfo] = useState<SymbolInfoData | null>(null);

  const symbolInfoRef = useRef<SymbolInfoData | null>(null);
  const fetchSymbolRef = useRef<(() => void) | null>(null);
  const isFetchingRef = useRef(false);

  useEffect(() => {
    symbolInfoRef.current = symbolInfo;
  }, [symbolInfo]);

  const inflightOrdersRef = useRef<Map<string, { side: Side, symbolId: number }>>(new Map());
  const orderMetadataRef = useRef<Map<string, { side: Side, symbolId: number }>>(new Map());
  const sentRequestsRef = useRef<Map<string, number>>(new Map());
  const nextExecId = useRef(BigInt(Date.now()));
  const nextOrderId = useRef(BigInt(Date.now()) * 1000n);

  const mgmtWsRef = useRef<WebSocket | null>(null);
  const mgmtRetryTimeoutRef = useRef<number | null>(null);
  const l2WsRef = useRef<WebSocket | null>(null);
  const l2RetryTimeoutRef = useRef<number | null>(null);
  const lastClientIdRef = useRef<string | null>(null);
  const notifiedExecIds = useRef<Set<string>>(new Set());
  const mgmtReadyNotifiedRef = useRef(false);
  const isInitialLoginRef = useRef(true);
  const clientSeqNumRef = useRef<bigint>(1n);
  const serverSeqNumRef = useRef<bigint>(0n);

  const bidsRef = useRef<Map<bigint, bigint>>(new Map());
  const asksRef = useRef<Map<bigint, bigint>>(new Map());
  const pricesRef = useRef<Map<number, bigint>>(new Map());
  const lastBookSymbolIdRef = useRef<number>(0);

  // Periodically flush market data refs to React state (throttle renders)
  useEffect(() => {
    const timer = setInterval(() => {
      setBids(new Map(bidsRef.current));
      setAsks(new Map(asksRef.current));
      setPrices(new Map(pricesRef.current));
    }, 500);
    return () => clearInterval(timer);
  }, []);

  const addMgmtLog = useCallback((msg: string) => {
    const timestamp = new Date().toLocaleTimeString('en-US', { hour12: false });
    const logEntry = `[${timestamp}] ${msg}`;
    setMgmtLogs(prev => [...prev, logEntry].slice(-100));
    console.log(`[Mgmt] ${logEntry}`);
  }, []);

  const addL2Log = useCallback((msg: string) => {
    console.log(`[L2] ${new Date().toLocaleTimeString()} - ${msg}`);
  }, []);

  // 1. Fetch SymbolInfo when activeSymbolId changes
  useEffect(() => {
    if (activeSymbolId <= 0 || isNaN(activeSymbolId)) return;
    
    let active = true;
    let retryTimeoutId: any = null;

    const fetchSymbol = async () => {
      if (retryTimeoutId) {
        clearTimeout(retryTimeoutId);
        retryTimeoutId = null;
      }
      if (isFetchingRef.current) return;
      isFetchingRef.current = true;

      try {
        const res = await fetch(`/v1/symbol/${activeSymbolId}`);
        if (!res.ok) {
          throw new Error(`HTTP error: ${res.status}`);
        }
        const arrayBuffer = await res.arrayBuffer();
        if (!active) return;
        
        const buf = new Uint8Array(arrayBuffer);
        const bb = new flatbuffers.ByteBuffer(buf);
        const info = SymbolInfo.getRootAsSymbolInfo(bb);
        
        const infoData: SymbolInfoData = {
          symbolId: info.symbolId(),
          name: info.name() || '',
          priceExp: info.priceExp(),
          priceMinStep: info.priceMinStep(),
          priceMin: info.priceMin(),
          priceMax: info.priceMax()
        };
        
        setSymbolInfo(infoData);
        const scale = Math.pow(10, -infoData.priceExp);
        const stepVal = Number(infoData.priceMinStep) / scale;
        const minVal = Number(infoData.priceMin) / scale;
        const maxVal = Number(infoData.priceMax) / scale;
        addMgmtLog(`Fetched SymbolInfo for ${infoData.name}: step=${stepVal}, min=${minVal}, max=${maxVal}`);
      } catch (err) {
        if (!active) return;
        addMgmtLog(`Failed to fetch SymbolInfo for symbol ${activeSymbolId}: ${err}. Retrying in 2 seconds...`);
        retryTimeoutId = setTimeout(fetchSymbol, 2000);
      } finally {
        isFetchingRef.current = false;
      }
    };
    
    fetchSymbolRef.current = fetchSymbol;
    fetchSymbol();
    
    return () => {
      active = false;
      if (retryTimeoutId) clearTimeout(retryTimeoutId);
      fetchSymbolRef.current = null;
    };
  }, [activeSymbolId, addMgmtLog]);

  // 2. Clear bids/asks and subscribe to L2 when activeSymbolId, connected.l2, or symbolInfo changes
  useEffect(() => {
    if (lastBookSymbolIdRef.current !== activeSymbolId) {
      setBids(new Map());
      setAsks(new Map());
      bidsRef.current.clear();
      asksRef.current.clear();
      lastBookSymbolIdRef.current = activeSymbolId;
    }

    if (activeSymbolId <= 0 || isNaN(activeSymbolId)) return;

    if (connected.l2 && symbolInfo && symbolInfo.symbolId === activeSymbolId) {
      if (l2WsRef.current?.readyState === WebSocket.OPEN) {
        setSubscribedSymbols(prev => {
          if (prev.has(activeSymbolId)) return prev;
          const builder = new flatbuffers.Builder(128);
          MarketDataRequest.startMarketDataRequest(builder);
          MarketDataRequest.addSymbolId(builder, activeSymbolId);
          MarketDataRequest.addMdType(builder, MDType.L2);
          MarketDataRequest.addSubType(builder, SubType.subscribe);
          const offset = MarketDataRequest.endMarketDataRequest(builder);
          MarketDataRequest.finishMarketDataRequestBuffer(builder, offset);
          l2WsRef.current!.send(builder.asUint8Array() as any);
          return new Set(prev).add(activeSymbolId);
        });
      }
    }
  }, [activeSymbolId, connected.l2, symbolInfo]);

  const subscribeL2 = useCallback((sId: number) => {
    setSubscribedSymbols(prev => {
      if (prev.has(sId)) return prev;
      if (l2WsRef.current?.readyState === WebSocket.OPEN) {
        const builder = new flatbuffers.Builder(128);
        MarketDataRequest.startMarketDataRequest(builder);
        MarketDataRequest.addSymbolId(builder, sId);
        MarketDataRequest.addMdType(builder, MDType.L2);
        MarketDataRequest.addSubType(builder, SubType.subscribe);
        const offset = MarketDataRequest.endMarketDataRequest(builder);
        MarketDataRequest.finishMarketDataRequestBuffer(builder, offset);
        l2WsRef.current.send(builder.asUint8Array() as any);
      }
      return new Set(prev).add(sId);
    });
  }, []);

  const handleOrderResponse = useCallback((resp: OrderResponse) => {
    const execType = resp.execType();
    const orderId = resp.orderId().toString();
    const execId = resp.execId().toString();
    const q = resp.q();
    const p = resp.p();
    const sId = resp.symbolId();
    const side = resp.side();
    const rejectCode = resp.rejectCode();

    if (execType !== ExecType.Complete && execType !== ExecType.OrderStatus && execType !== ExecType.Cancelled && symbolInfoRef.current && sId === activeSymbolId) {
      if (p !== 0n) {
        const valErr = validatePrice(p, symbolInfoRef.current);
        if (valErr) {
          addMgmtLog(`[Error] Received invalid OrderResponse price: ${valErr}`);
          onNotification?.('rejected', 'Response Price Invalid', valErr);
        }
      }
    }

    if (execType === ExecType.Complete) {
      setConnected(prev => ({ ...prev, mgmtReady: true }));
      addMgmtLog('[System] Management session ready');
      if (!mgmtReadyNotifiedRef.current) {
        if (isInitialLoginRef.current) {
          onNotification?.('info', 'System', 'Log in success');
          isInitialLoginRef.current = false;
        }
        mgmtReadyNotifiedRef.current = true;
      }
      return;
    }
    
    const sentTime = sentRequestsRef.current.get(execId);
    let latencyStr = '';
    if (sentTime !== undefined) {
      const rttMs = performance.now() - sentTime;
      const engineLat = 0;
      const managerLat = 0;
      latencyStr = ` Latency=${rttMs.toFixed(3)}ms`;
      if (engineLat > 0n || managerLat > 0n) {
        latencyStr += ` (Engine=${engineLat}cyc, Mgr=${managerLat}cyc)`;
      }
      sentRequestsRef.current.delete(execId);
    }

    const execName = ExecType[execType] ?? `Unknown(${execType})`;
    addMgmtLog(`[Exec] ID=${orderId} Type=${execName} Side=${Side[side]} P=${p} Q=${q} ExecID=${execId}${latencyStr}`);

    // Deduplicate notifications by execId
    const shouldNotify = execId !== '0' && !notifiedExecIds.current.has(execId);
    if (execId !== '0') {
      notifiedExecIds.current.add(execId);
    }

    if (rejectCode !== 0) {
      const msg = REJECT_MESSAGES[rejectCode] || `Error Code: ${rejectCode}`;
      addMgmtLog(`[Error] Order Rejected: ID=${orderId} Code=${rejectCode} (${msg})${latencyStr}`);
      if (shouldNotify) {
        onNotification?.('rejected', 'Order Rejected', `${msg} (ID: ${orderId})`);
      }
      return;
    }

    if (shouldNotify) {
      if (execType === ExecType.New) {
        onNotification?.('acked', 'Order Accepted', `${Side[side]} ${q} @ ${p} (ID: ${orderId})`);
      } else if (execType === ExecType.Cancelled) {
        onNotification?.('info', 'Order Cancelled', `ID: ${orderId} has been removed`);
      } else if (execType === ExecType.Replaced) {
        onNotification?.('acked', 'Order Modified', `ID: ${orderId} updated to Qty: ${q}`);
      } else if (execType === ExecType.Fill) {
        onNotification?.('acked', 'Order Filled', `${Side[side]} ${q} @ ${p} (ID: ${orderId})`);
      } else if (execType === ExecType.PartialFill) {
        onNotification?.('info', 'Partial Fill', `${Side[side]} ${q} @ ${p} (ID: ${orderId})`);
      }
    }

    if (orderId !== '0') {
      orderMetadataRef.current.set(orderId, { side, symbolId: sId });
    }

    if (execType === ExecType.New || execType === ExecType.OrderStatus) {
      setOpenOrders(prev => {
        const next = new Map(prev);
        if (!next.has(orderId)) {
          next.set(orderId, { orderId, symbolId: sId, side, p, q, filled: 0n });
        }
        return next;
      });
    } else if (execType === ExecType.Replaced) {
      setOpenOrders(prev => {
        const next = new Map(prev);
        const existing = next.get(orderId);
        next.set(orderId, { orderId, symbolId: sId, side, p, q, filled: existing ? existing.filled : 0n });
        return next;
      });
    } else if (execType === ExecType.PartialFill || execType === ExecType.Fill) {
      setOpenOrders(prev => {
        const next = new Map(prev);
        const existing = next.get(orderId);
        const order = existing 
          ? { ...existing, filled: existing.filled + q }
          : { orderId, symbolId: sId, side, p, q: 0n, filled: q };
          
        if (execType === ExecType.Fill) {
          next.delete(orderId);
        } else {
          next.set(orderId, order);
        }
        return next;
      });

      if (side !== Side.None) {
        const fillQty = q;
        const fillPrice = p;
        const cashValue = q * p;
        const cashDelta = side === Side.Buy ? -cashValue : cashValue;
        
        setCash(prev => prev + cashDelta);
        
        setPositions(pPrev => {
          const pNext = new Map(pPrev);
          let currentPos = pNext.get(sId);
          if (!currentPos) {
            currentPos = {
              symbolId: sId,
              side: Side.None,
              lots: [],
              totalQuantity: 0n,
              averagePrice: 0n,
              realizedPnL: 0n,
            };
          }

          const nextPos: SymbolPosition = { ...currentPos, lots: currentPos.lots.map(l => ({...l})) };
          let remainingFillQty = fillQty;

          if (nextPos.side === Side.None || nextPos.side === side) {
            nextPos.side = side;
            nextPos.lots.push({ price: fillPrice, quantity: fillQty, timestamp: Date.now(), orderId });
            nextPos.totalQuantity += fillQty;
          } else {
            // Opposite side trade: apply FIFO
            while (remainingFillQty > 0n && nextPos.lots.length > 0) {
              const oldestLot = nextPos.lots[0];
              if (remainingFillQty >= oldestLot.quantity) {
                const closedQty = oldestLot.quantity;
                const pnl = nextPos.side === Side.Buy 
                  ? (fillPrice - oldestLot.price) * closedQty 
                  : (oldestLot.price - fillPrice) * closedQty;
                
                nextPos.realizedPnL += pnl;
                remainingFillQty -= closedQty;
                nextPos.totalQuantity -= closedQty;
                nextPos.lots.shift();
              } else {
                const closedQty = remainingFillQty;
                const pnl = nextPos.side === Side.Buy 
                  ? (fillPrice - oldestLot.price) * closedQty 
                  : (oldestLot.price - fillPrice) * closedQty;

                nextPos.realizedPnL += pnl;
                oldestLot.quantity -= closedQty;
                nextPos.totalQuantity -= closedQty;
                remainingFillQty = 0n;
              }
            }

            if (remainingFillQty > 0n) {
              nextPos.side = side;
              nextPos.lots.push({ price: fillPrice, quantity: remainingFillQty, timestamp: Date.now(), orderId });
              nextPos.totalQuantity = remainingFillQty;
            } else if (nextPos.lots.length === 0) {
              nextPos.side = Side.None;
              nextPos.totalQuantity = 0n;
            }
          }

          // Update average price
          if (nextPos.totalQuantity > 0n) {
            const totalCost = nextPos.lots.reduce((acc, lot) => acc + lot.price * lot.quantity, 0n);
            nextPos.averagePrice = totalCost / nextPos.totalQuantity;
          } else {
            nextPos.averagePrice = 0n;
          }

          pNext.set(sId, nextPos);
          return pNext;
        });
      }
    } else if (execType === ExecType.Cancelled) {
      setOpenOrders(prev => {
        const next = new Map(prev);
        next.delete(orderId);
        return next;
      });
    }
  }, [addMgmtLog, onNotification]);

  const connectMgmt = useCallback((clientId: string, symbolId: string) => {
    if (mgmtRetryTimeoutRef.current) {
      clearTimeout(mgmtRetryTimeoutRef.current);
      mgmtRetryTimeoutRef.current = null;
    }
    if (mgmtWsRef.current) mgmtWsRef.current.close();
    
    lastClientIdRef.current = clientId;
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${protocol}//${window.location.host}/ws-mgmt`;
    const numericClientId = hashClientId(clientId);
    addMgmtLog(`Connecting to Management WS... ClientID=${clientId}`);
    
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    mgmtWsRef.current = ws;

    ws.onopen = () => {
      setOpenOrders(new Map());
      // On re-open, we keep positions to avoid flickering but we will sync net qty
      notifiedExecIds.current.clear();
      sentRequestsRef.current.clear();
      mgmtReadyNotifiedRef.current = false;
      addMgmtLog(`Connected ClientID=${clientId}`);
      setConnected(prev => ({ ...prev, mgmt: true }));
      const builder = new flatbuffers.Builder(256);
      const usernameOffset = builder.createString(clientId);
      AdminRequest.startAdminRequest(builder);
      AdminRequest.addAction(builder, AdminAction.LogOn);
      AdminRequest.addClientId(builder, numericClientId);
      AdminRequest.addUsername(builder, usernameOffset);
      AdminRequest.addMsgSeqNum(builder, clientSeqNumRef.current);
      AdminRequest.addAckSeqNum(builder, serverSeqNumRef.current);
      const adminReqOffset = AdminRequest.endAdminRequest(builder);

      ClientRequest.startClientRequest(builder);
      ClientRequest.addDataType(builder, ClientReqData.AdminRequest);
      ClientRequest.addData(builder, adminReqOffset);
      const clientReqOffset = ClientRequest.endClientRequest(builder);

      builder.finish(clientReqOffset);
      ws.send(builder.asUint8Array() as any);
    };

    ws.onmessage = (event) => {
      try {
        const buf = new Uint8Array(event.data);
        const bb = new flatbuffers.ByteBuffer(buf);
        const response = ClientResponse.getRootAsClientResponse(bb);
        const dataType = response.dataType();
        if (dataType === ClientResponseData.OrderResponse) {
          const orderResp = response.data(new OrderResponse()) as OrderResponse;
          if (orderResp) handleOrderResponse(orderResp);
        } else if (dataType === ClientResponseData.PositionResponse) {
          const posResp = response.data(new PositionResponse()) as PositionResponse;
          if (posResp) {
            const sId = posResp.symbolId();
            const qty = posResp.position();
            if (sId === 0) {
              setCash(qty);
            } else {
              setPositions(prev => {
                const next = new Map(prev);
                let current = next.get(sId);
                const side = qty > 0n ? Side.Buy : (qty < 0n ? Side.Sell : Side.None);
                const absQty = qty > 0n ? qty : -qty;

                if (!current) {
                  current = {
                    symbolId: sId,
                    side: side,
                    lots: qty !== 0n ? [{ price: 0n, quantity: absQty, timestamp: Date.now(), orderId: 'sync' }] : [],
                    totalQuantity: absQty,
                    averagePrice: 0n,
                    realizedPnL: 0n,
                  };
                } else {
                  // Re-sync logic: server provides net position. 
                  // If we're reconnecting, we maintain the net total but consolidate into a single 'sync' lot 
                  // because we can't reliably reconcile multiple lots from a net position sync.
                  if (current.totalQuantity !== absQty || current.side !== side) {
                    current.totalQuantity = absQty;
                    current.side = side;
                    current.lots = qty !== 0n ? [{ price: current.averagePrice, quantity: absQty, timestamp: Date.now(), orderId: 'sync' }] : [];
                  }
                }
                next.set(sId, current);
                return next;
              });
            }
            addMgmtLog(`Position Sync: Sym=${sId} Qty=${qty}`);
            if (sId !== 0) subscribeL2(sId);
          }
        } else if (dataType === ClientResponseData.AdminResponse) {
          const adminResp = response.data(new AdminResponse()) as AdminResponse;
          if (adminResp && adminResp.type() === AdminResponseType.Ready) {
            setConnected(prev => ({ ...prev, mgmtReady: true }));
            if (!mgmtReadyNotifiedRef.current) {
              if (isInitialLoginRef.current) {
                onNotification?.('info', 'System', 'Log in success');
                isInitialLoginRef.current = false;
              }
              mgmtReadyNotifiedRef.current = true;
            }

            addMgmtLog(`Admin Ready received. Requesting open orders and positions...`);
            
            const builder = new flatbuffers.Builder(256);
            
            // Request open orders
            OpenOrderRequest.startOpenOrderRequest(builder);
            OpenOrderRequest.addClientId(builder, numericClientId);
            const openReqOffset = OpenOrderRequest.endOpenOrderRequest(builder);
            
            ClientRequest.startClientRequest(builder);
            ClientRequest.addDataType(builder, ClientReqData.OpenOrderRequest);
            ClientRequest.addData(builder, openReqOffset);
            const clientReqOffset = ClientRequest.endClientRequest(builder);
            builder.finish(clientReqOffset);
            ws.send(builder.asUint8Array() as any);
            
            // Request positions (for all cached symbols)
            const cachedSymbols = [0];
            if (activeSymbolId > 0 && !isNaN(activeSymbolId)) {
                cachedSymbols.push(activeSymbolId);
            }
            for (const sym of cachedSymbols) {
               builder.clear();
               PositionRequest.startPositionRequest(builder);
               PositionRequest.addClientId(builder, numericClientId);
               PositionRequest.addSymbolId(builder, sym);
               const posReqOffset = PositionRequest.endPositionRequest(builder);
               
               ClientRequest.startClientRequest(builder);
               ClientRequest.addDataType(builder, ClientReqData.PositionRequest);
               ClientRequest.addData(builder, posReqOffset);
               const crOffset = ClientRequest.endClientRequest(builder);
               builder.finish(crOffset);
               ws.send(builder.asUint8Array() as any);
            }
          } else if (adminResp && adminResp.type() === AdminResponseType.Reject) {
            const reasonCode = adminResp.rejectCode();
            addMgmtLog(`Admin Login Rejected: code=${RejectCode[reasonCode] || reasonCode}`);
            
            if (reasonCode === RejectCode.InvalidSequenceNumber) {
                clientSeqNumRef.current = adminResp.expectedMsgSeqNum();
                serverSeqNumRef.current = adminResp.expectedAckSeqNum();
                
                addMgmtLog(`Resyncing seq numbers to Msg=${clientSeqNumRef.current}, Ack=${serverSeqNumRef.current} and retrying LogOn`);
                const builder = new flatbuffers.Builder(256);
                const usernameOffset = builder.createString(clientId);
                AdminRequest.startAdminRequest(builder);
                AdminRequest.addAction(builder, AdminAction.LogOn);
                AdminRequest.addClientId(builder, numericClientId);
                AdminRequest.addUsername(builder, usernameOffset);
                AdminRequest.addMsgSeqNum(builder, clientSeqNumRef.current);
                AdminRequest.addAckSeqNum(builder, serverSeqNumRef.current);
                const adminReqOffset = AdminRequest.endAdminRequest(builder);

                ClientRequest.startClientRequest(builder);
                ClientRequest.addDataType(builder, ClientReqData.AdminRequest);
                ClientRequest.addData(builder, adminReqOffset);
                const clientReqOffset = ClientRequest.endClientRequest(builder);

                builder.finish(clientReqOffset);
                ws.send(builder.asUint8Array() as any);
            }
          }
        }
      } catch (err) { addMgmtLog(`Decode Error: ${err}`); }
    };

    ws.onclose = (e) => { 
      addMgmtLog(`Disconnected (Code: ${e.code}). Retrying in 2s...`); 
      setConnected(prev => ({ ...prev, mgmt: false, mgmtReady: false })); 
      mgmtWsRef.current = null; 
      if (lastClientIdRef.current) {
        mgmtRetryTimeoutRef.current = window.setTimeout(() => connectMgmt(lastClientIdRef.current!, symbolId), 2000);
      }
    };

    ws.onerror = () => addMgmtLog(`WebSocket Error`);
  }, [addMgmtLog, handleOrderResponse, subscribeL2]);

  const connectL2 = useCallback(() => {
    if (l2RetryTimeoutRef.current) {
      clearTimeout(l2RetryTimeoutRef.current);
      l2RetryTimeoutRef.current = null;
    }
    if (l2WsRef.current) l2WsRef.current.close();
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${protocol}//${window.location.host}/marketdata`;
    addL2Log(`Connecting to Market Data WS (9002)...`);
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    l2WsRef.current = ws;

    ws.onopen = () => {
      setConnected(prev => ({ ...prev, l2: true }));
      addL2Log('Connected');
      // Resubscribe to previous symbols if any
      setSubscribedSymbols(prev => {
        const next = new Set(prev);
        if (next.size === 0) {
          next.add(1);
        }
        next.forEach(sId => {
          const builder = new flatbuffers.Builder(128);
          MarketDataRequest.startMarketDataRequest(builder);
          MarketDataRequest.addSymbolId(builder, sId);
          MarketDataRequest.addMdType(builder, MDType.L2);
          MarketDataRequest.addSubType(builder, SubType.subscribe);
          const offset = MarketDataRequest.endMarketDataRequest(builder);
          MarketDataRequest.finishMarketDataRequestBuffer(builder, offset);
          ws.send(builder.asUint8Array() as any);
        });
        return next;
      });
    };

    ws.onmessage = (event) => {
      if (ws.readyState !== WebSocket.OPEN) return;
      try {
        const buf = new Uint8Array(event.data);
        const bb = new flatbuffers.ByteBuffer(buf);
        const update = MarketDataUpdate.getRootAsMarketDataUpdate(bb);
        if (update.dataType() !== MarketDataUpdateData.L2Update) return;
        const l2Update = update.data(new L2Update()) as L2Update;
        if (!l2Update) return;
        const side = l2Update.side(); 
        const p = l2Update.p(); 
        const q = l2Update.q();
        const sId = l2Update.symbolId();

        if (sId === activeSymbolId && (!symbolInfoRef.current || symbolInfoRef.current.symbolId !== sId)) {
          if (fetchSymbolRef.current) {
            addMgmtLog(`[L2] Snapshot/update received but symbol data not yet retrieved. Fetching SymbolInfo immediately...`);
            fetchSymbolRef.current();
          }
        }

        if (side !== Side.None && q !== BigInt(0) && symbolInfoRef.current && sId === activeSymbolId) {
          const valErr = validatePrice(p, symbolInfoRef.current);
          if (valErr) {
            addMgmtLog(`[Error] Received invalid L2 price: ${valErr}`);
            onNotification?.('rejected', 'L2 Price Invalid', valErr);
          }
        }

        if (side === Side.None) { 
          if (sId === activeSymbolId) {
            setBids(new Map());
            setAsks(new Map());
            bidsRef.current.clear();
            asksRef.current.clear();
          }
          return; 
        }

        if (side === Side.Buy) {
          if (sId === activeSymbolId) {
            if (q === BigInt(0)) {
              bidsRef.current.delete(p);
            } else {
              bidsRef.current.set(p, q);
            }
          }
          
          // Always update prices for total value calculation
          if (q > 0n) {
            pricesRef.current.set(sId, p);
          }
        } else if (side === Side.Sell) {
          if (sId === activeSymbolId) {
            if (q === BigInt(0)) {
              asksRef.current.delete(p);
            } else {
              asksRef.current.set(p, q);
            }
          }
        }

        // Price cross check
        if (sId === activeSymbolId) {
          let bestBid: bigint | null = null;
          for (const price of bidsRef.current.keys()) {
            if (bestBid === null || price > bestBid) {
              bestBid = price;
            }
          }

          let bestAsk: bigint | null = null;
          for (const price of asksRef.current.keys()) {
            if (bestAsk === null || price < bestAsk) {
              bestAsk = price;
            }
          }

          if (bestBid !== null && bestAsk !== null && bestBid >= bestAsk) {
            addMgmtLog(`[Error] Orderbook crossed! Best Bid: ${bestBid} >= Best Ask: ${bestAsk}. Reconnecting L2 WS...`);
            onNotification?.('rejected', 'Orderbook Crossed', `Best Bid ${bestBid} >= Best Ask ${bestAsk}. Reconnecting...`);
            
            // Clear book state to prevent duplicate/stale checks before reconnection
            bidsRef.current.clear();
            asksRef.current.clear();
            setBids(new Map());
            setAsks(new Map());

            ws.close();
          }
        }
      } catch (err) { addL2Log(`Decode Error: ${err}`); }
    };
    ws.onclose = (e) => { 
      addL2Log(`Disconnected (Code: ${e.code}). Retrying in 2s...`); 
      setConnected(prev => ({ ...prev, l2: false })); 
      l2WsRef.current = null; 
      l2RetryTimeoutRef.current = window.setTimeout(connectL2, 2000);
    };
    ws.onerror = () => addL2Log(`WebSocket Error`);
  }, [addL2Log, activeSymbolId]);

  useEffect(() => {
    connectL2();
    return () => {
      if (l2WsRef.current) l2WsRef.current.close();
      if (l2RetryTimeoutRef.current) clearTimeout(l2RetryTimeoutRef.current);
    };
  }, [connectL2]);

  const disconnectAll = useCallback(() => {
    mgmtWsRef.current?.close();
    l2WsRef.current?.close();
    if (l2RetryTimeoutRef.current) {
      clearTimeout(l2RetryTimeoutRef.current);
      l2RetryTimeoutRef.current = null;
    }
  }, []);

  const sendOrder = useCallback(async (side: Side, clientId: string, symbolId: string, price: string, quantity: string, type: OrderType = OrderType.Limit) => {
    if (!mgmtWsRef.current || mgmtWsRef.current.readyState !== WebSocket.OPEN) {
      addMgmtLog(`Error: Management WS not connected`); return;
    }
    const builder = new flatbuffers.Builder(1024);
    const qVal = BigInt(quantity); const execId = nextExecId.current++;
    const orderId = nextOrderId.current++;
    const numericClientId = hashClientId(clientId);
    inflightOrdersRef.current.set(execId.toString(), { side, symbolId: parseInt(symbolId) });
    OrderRequest.startOrderRequest(builder);
    OrderRequest.addAction(builder, OrderAction.New);
    OrderRequest.addExecId(builder, execId);
    OrderRequest.addOrderId(builder, orderId);
    OrderRequest.addClientId(builder, numericClientId);
    OrderRequest.addSymbolId(builder, parseInt(symbolId));
    OrderRequest.addSide(builder, side);
    OrderRequest.addType(builder, type);
    
    const pVal = parsePrice(price, symbolInfo?.priceExp);
    if (type !== OrderType.Market) {
      const valErr = validatePrice(pVal, symbolInfo);
      if (valErr) {
        addMgmtLog(`[Error] Order rejected locally: ${valErr}`);
        onNotification?.('rejected', 'Invalid Price', valErr);
        return;
      }
    }
    OrderRequest.addP(builder, pVal);
    
    OrderRequest.addQ(builder, qVal);
    OrderRequest.addVisibleQty(builder, qVal);
    OrderRequest.addTimestamp(builder, BigInt(Date.now()));
    
    clientSeqNumRef.current += 1n;
    OrderRequest.addMsgSeqNum(builder, clientSeqNumRef.current);

    const off = OrderRequest.endOrderRequest(builder);
    ClientRequest.startClientRequest(builder);
    ClientRequest.addDataType(builder, ClientReqData.OrderRequest);
    ClientRequest.addData(builder, off);
    builder.finish(ClientRequest.endClientRequest(builder));
    try {
      addMgmtLog(`Sending ${Side[side]} ${OrderType[type]} order: ClientID=${clientId}(${numericClientId}) P=${price} (raw=${pVal}) Q=${quantity} ID=${orderId} ExecID=${execId}`);
      sentRequestsRef.current.set(execId.toString(), performance.now());
      mgmtWsRef.current.send(builder.asUint8Array() as any);
    } catch (err) { addMgmtLog(`Order send error: ${err}`); }
  }, [addMgmtLog, symbolInfo]);

  const cancelOrder = useCallback((order: OrderData, clientId: string) => {
    if (!mgmtWsRef.current || mgmtWsRef.current.readyState !== WebSocket.OPEN) return;
    const builder = new flatbuffers.Builder(1024);
    const execId = nextExecId.current++;
    const numericClientId = hashClientId(clientId);
    OrderRequest.startOrderRequest(builder);
    OrderRequest.addAction(builder, OrderAction.Cancel);
    OrderRequest.addExecId(builder, execId);
    OrderRequest.addOrderId(builder, BigInt(order.orderId));
    OrderRequest.addClientId(builder, numericClientId);
    OrderRequest.addSymbolId(builder, order.symbolId);
    OrderRequest.addSide(builder, order.side);
    OrderRequest.addTimestamp(builder, BigInt(Date.now()));

    clientSeqNumRef.current += 1n;
    OrderRequest.addMsgSeqNum(builder, clientSeqNumRef.current);

    const off = OrderRequest.endOrderRequest(builder);
    ClientRequest.startClientRequest(builder);
    ClientRequest.addDataType(builder, ClientReqData.OrderRequest);
    ClientRequest.addData(builder, off);
    builder.finish(ClientRequest.endClientRequest(builder));
    addMgmtLog(`Cancelling Order: ClientID=${clientId}(${numericClientId}) ID=${order.orderId} ExecID=${execId}`)
    sentRequestsRef.current.set(execId.toString(), performance.now());
    mgmtWsRef.current.send(builder.asUint8Array() as any)
  }, [addMgmtLog]);

  const modifyOrder = useCallback((order: OrderData, clientId: string, newPrice: string, newQty: string) => {
    if (!mgmtWsRef.current || mgmtWsRef.current.readyState !== WebSocket.OPEN) return;
    const builder = new flatbuffers.Builder(1024);
    const execId = nextExecId.current++;
    const numericClientId = hashClientId(clientId);
    OrderRequest.startOrderRequest(builder);
    OrderRequest.addAction(builder, OrderAction.Modify);
    OrderRequest.addExecId(builder, execId);
    OrderRequest.addOrderId(builder, BigInt(order.orderId));
    OrderRequest.addClientId(builder, numericClientId);
    OrderRequest.addSymbolId(builder, order.symbolId);
    OrderRequest.addSide(builder, order.side);
    
    const pVal = parsePrice(newPrice, symbolInfo?.priceExp);
    const valErr = validatePrice(pVal, symbolInfo);
    if (valErr) {
      addMgmtLog(`[Error] Modify rejected locally: ${valErr}`);
      onNotification?.('rejected', 'Invalid Price', valErr);
      return;
    }
    OrderRequest.addP(builder, pVal);
    
    OrderRequest.addQ(builder, BigInt(newQty));
    OrderRequest.addVisibleQty(builder, BigInt(newQty));
    OrderRequest.addTimestamp(builder, BigInt(Date.now()));

    clientSeqNumRef.current += 1n;
    OrderRequest.addMsgSeqNum(builder, clientSeqNumRef.current);

    const off = OrderRequest.endOrderRequest(builder);
    ClientRequest.startClientRequest(builder);
    ClientRequest.addDataType(builder, ClientReqData.OrderRequest);
    ClientRequest.addData(builder, off);
    builder.finish(ClientRequest.endClientRequest(builder));
    addMgmtLog(`Modifying Order: ClientID=${clientId}(${numericClientId}) ID=${order.orderId} NewP=${newPrice} (raw=${pVal}) NewQ=${newQty} ExecID=${execId}`)
    sentRequestsRef.current.set(execId.toString(), performance.now());
    mgmtWsRef.current.send(builder.asUint8Array() as any)
  }, [addMgmtLog, symbolInfo]);

  const clearMgmtLogs = useCallback(() => {
    setMgmtLogs([]);
  }, []);

  return {
    connected,
    bids,
    asks,
    prices,
    openOrders,
    positions,
    cash,
    connectMgmt,
    connectL2,
    subscribeL2,
    disconnectAll,
    sendOrder,
    cancelOrder,
    modifyOrder,
    mgmtLogs,
    clearMgmtLogs,
    symbolInfo
  };
}
