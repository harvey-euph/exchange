import { useState, useRef, useMemo, useEffect, useCallback } from 'react';
import { Side } from './fbs/exchange/side';
import { OrderType } from './fbs/exchange/order-type';
import { useExchange } from './hooks/useExchange';
import { OrderBook } from './components/OrderBook';
import { OrderEntry } from './components/OrderEntry';
import { OpenOrders } from './components/OpenOrders';
import { Positions } from './components/Positions';
import { EmbeddedLog } from './components/EmbeddedLog';
import { NotificationSystem } from './components/NotificationSystem';
import type { NotificationSystemRef } from './components/NotificationSystem';
import { formatPrice } from './types';
import './App.css';

function App() {
  const [clientId, setClientId] = useState('101');
  const [symbolId, setSymbolId] = useState('1');
  const [price, setPrice] = useState('5000');
  const [quantity, setQuantity] = useState('100');
  const [side, setSide] = useState<Side>(Side.Buy);
  const [peggedLevel, setPeggedLevel] = useState<number | null>(null);
  const [hasLoggedIn, setHasLoggedIn] = useState(false);
  const [activeTab, setActiveTab] = useState<'orders' | 'positions'>('orders');
  const [expandedSymbols, setExpandedSymbols] = useState<Set<number>>(new Set());
  
  const notifRef = useRef<NotificationSystemRef>(null);

  const handleNotification = useCallback((type: 'acked' | 'rejected' | 'info', title: string, content: string) => {
    notifRef.current?.addNotification(type, title, content);
  }, []);

  const {
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
    sendOrder,
    cancelOrder,
    modifyOrder,
    mgmtLogs,
    clearMgmtLogs,
    symbolInfo
  } = useExchange(parseInt(symbolId), handleNotification);

  // Set default prices based on the selected symbol info
  useEffect(() => {
    if (symbolInfo) {
      if (symbolInfo.symbolId === 1) setPrice('50000.00');
      else if (symbolInfo.symbolId === 2) setPrice('3000.00');
      else if (symbolInfo.symbolId === 3) setPrice('150.00');
    }
  }, [symbolInfo]);

  // Track initial successful connection to disable inputs
  useEffect(() => {
    if (connected.mgmtReady) {
      setHasLoggedIn(true);
    }
  }, [connected.mgmtReady]);

  // Auto-expand current symbol
  useEffect(() => {
    if (symbolId !== undefined) {
      const sid = parseInt(symbolId);
      if (!isNaN(sid)) {
        setExpandedSymbols(prev => new Set([...prev, sid]));
      }
    }
  }, [symbolId]);

  const sortedAsks = useMemo(() => 
    Array.from(asks.entries())
      .map(([p, q]) => ({ price: p, quantity: q }))
      .sort((a, b) => (a.price > b.price ? -1 : 1)), 
  [asks]);

  const sortedBids = useMemo(() => 
    Array.from(bids.entries())
      .map(([p, q]) => ({ price: p, quantity: q }))
      .sort((a, b) => (a.price > b.price ? -1 : 1)), 
  [bids]);

  const handlePriceClick = (newPrice: string, newSide: Side, level: number | null = null) => {
    setPrice(newPrice);
    setSide(newSide);
    setPeggedLevel(level);
  };

  const handleSendOrder = (orderSide: Side) => {
    if (!connected.mgmtReady) {
      handleNotification('rejected', 'Error', 'Please login first');
      return;
    }

    let finalPrice = price;
    if (peggedLevel !== null) {
      // Resolve pegged price at the moment of send.
      // sortedBids/sortedAsks carry raw mantissa bigints — use formatPrice so
      // parsePrice inside sendOrder applies priceExp exactly once.
      if (orderSide === Side.Buy) {
        const target = sortedBids[peggedLevel - 1]?.price;
        if (target !== undefined) {
          finalPrice = formatPrice(target, symbolInfo?.priceExp);
        } else {
          handleNotification('rejected', 'Error', `No level BID ${peggedLevel} to peg to`);
          return;
        }
      } else {
        const target = sortedAsks[sortedAsks.length - peggedLevel]?.price;
        if (target !== undefined) {
          finalPrice = formatPrice(target, symbolInfo?.priceExp);
        } else {
          handleNotification('rejected', 'Error', `No level ASK ${peggedLevel} to peg to`);
          return;
        }
      }
    }

    sendOrder(orderSide, clientId, symbolId, finalPrice, quantity);
  };
  
  const handleCancelOrder = (order: any) => cancelOrder(order, clientId);
  const handleModifyOrder = (order: any, newPrice: string, newQty: string) => modifyOrder(order, clientId, newPrice, newQty);

  const handleFlatten = useCallback((sId: number, flattenSide: Side, flattenQuantity: bigint) => {
    if (!connected.mgmtReady) {
      handleNotification('rejected', 'Error', 'Please login first');
      return;
    }
    const oppositeSide = flattenSide === Side.Buy ? Side.Sell : Side.Buy;
    const markPrice = prices.get(sId) || 0n;
    sendOrder(oppositeSide, clientId, sId.toString(), markPrice.toString(), flattenQuantity.toString(), OrderType.Market);
  }, [connected.mgmtReady, clientId, prices, sendOrder, handleNotification]);

  const handleLogin = () => {
    connectMgmt(clientId, symbolId);
  };

  const totalValue = useMemo(() => {
    let value = cash;
    for (const [sId, pos] of positions) {
      const price = prices.get(sId) || pos.averagePrice || 0n;
      const posValue = pos.side === Side.Buy ? pos.totalQuantity * price : -pos.totalQuantity * price;
      value += posValue;
    }
    return value;
  }, [positions, cash, prices]);

  const toggleSymbol = (sid: number) => {
    setExpandedSymbols(prev => {
      const next = new Set(prev);
      if (next.has(sid)) next.delete(sid);
      else next.add(sid);
      return next;
    });
  };

  const currentTabSymbols = useMemo(() => {
    if (activeTab === 'orders') {
      const symbols = new Set<number>();
      openOrders.forEach(o => symbols.add(o.symbolId));
      return Array.from(symbols);
    } else {
      return Array.from(positions.keys()).filter(sid => {
        const p = positions.get(sid);
        return p && (p.totalQuantity !== 0n || p.realizedPnL !== 0n);
      });
    }
  }, [activeTab, openOrders, positions]);

  const isAllExpanded = currentTabSymbols.length > 0 && currentTabSymbols.every(s => expandedSymbols.has(s));

  const handleToggleExpandAll = () => {
    if (isAllExpanded) {
      setExpandedSymbols(new Set());
    } else {
      setExpandedSymbols(new Set(currentTabSymbols));
    }
  };

  return (
    <div className="app-container">
      <NotificationSystem ref={notifRef} />
      
      <header className="header-container">
        <div className="header-left">
          <div style={{ display: 'flex', alignItems: 'center', gap: '10px' }}>
            <div style={{ width: '28px', height: '28px', backgroundColor: 'var(--accent-blue)', borderRadius: '6px', display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#fff', fontWeight: 'bold', fontSize: '16px' }}>H</div>
            <h1 style={{ margin: 0, fontSize: '18px', fontWeight: 700, letterSpacing: '-0.5px' }}>Harvey Exchange</h1>
          </div>
        </div>

        <div className="header-right">
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
            <div style={{ width: '6px', height: '6px', borderRadius: '50%', backgroundColor: connected.l2 ? 'var(--accent-green)' : 'var(--accent-red)' }} />
            <span style={{ fontSize: '11px', color: 'var(--text-secondary)' }}>Market Data</span>
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
            <div style={{ width: '6px', height: '6px', borderRadius: '50%', backgroundColor: connected.mgmtReady ? 'var(--accent-green)' : 'var(--accent-red)' }} />
            <span style={{ fontSize: '11px', color: 'var(--text-secondary)' }}>Client Server</span>
          </div>
        </div>
      </header>

      <main className="main-content">
        <OrderBook 
          symbolId={symbolId} onSymbolChange={setSymbolId}
          bids={sortedBids} asks={sortedAsks} onPriceClick={handlePriceClick} 
          onReconnectL2={connectL2}
          priceExp={symbolInfo?.priceExp}
        />

        <div className="right-panel">
          <div className="layout-columns">
            <div className="left-col">
              <div className="modern-card tab-container">
                <div className="tab-header" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                  <div style={{ display: 'flex', gap: '20px' }}>
                    <button 
                      className={`tab-item ${activeTab === 'orders' ? 'active' : ''}`}
                      onClick={() => setActiveTab('orders')}
                    >
                      OPEN ORDERS
                    </button>
                    <button 
                      className={`tab-item ${activeTab === 'positions' ? 'active' : ''}`}
                      onClick={() => setActiveTab('positions')}
                    >
                      POSITIONS
                    </button>
                  </div>
                  
                  <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                      <span style={{ fontSize: '11px', color: 'var(--text-secondary)' }}>NAV:</span>
                      <span style={{ fontSize: '13px', color: 'var(--text-primary)', fontWeight: 'bold', fontFamily: 'var(--font-mono)' }}>
                        {formatPrice(totalValue, symbolInfo?.priceExp)}
                      </span>
                    </div>
                    <button 
                      className="modern-button btn-secondary" 
                      onClick={handleToggleExpandAll}
                      style={{ padding: '2px 8px', fontSize: '10px', height: '22px', minWidth: '85px' }}
                    >
                      {isAllExpanded ? 'Collapse All' : 'Expand All'}
                    </button>
                  </div>
                </div>
                <div className="tab-content">
                  {activeTab === 'orders' ? (
                    <OpenOrders 
                      orders={Array.from(openOrders.values())}
                      onModify={handleModifyOrder}
                      onCancel={handleCancelOrder}
                      currentSymbolId={symbolId}
                      noWrapper
                      expandedSymbols={expandedSymbols}
                      onToggleSymbol={toggleSymbol}
                      symbolInfo={symbolInfo}
                    />
                  ) : (
                    <Positions 
                      positions={Array.from(positions.entries())} 
                      prices={prices} 
                      currentSymbolId={symbolId}
                      onFlatten={handleFlatten}
                      noWrapper
                      expandedSymbols={expandedSymbols}
                      onToggleSymbol={toggleSymbol}
                    />
                  )}
                </div>
              </div>
            </div>
            <div className="right-col">
              <OrderEntry 
                isLoggedIn={hasLoggedIn}
                clientId={clientId}
                setClientId={setClientId}
                onLogin={handleLogin}
                price={price} quantity={quantity} side={side} peggedLevel={peggedLevel}
                setPrice={setPrice} setQuantity={setQuantity} setSide={setSide} setPeggedLevel={setPeggedLevel}
                onSendOrder={handleSendOrder}
                cash={cash}
                disabled={!connected.mgmtReady}
                priceExp={symbolInfo?.priceExp}
                priceMinStep={symbolInfo?.priceMinStep}
              />
              <EmbeddedLog logs={mgmtLogs} onClear={clearMgmtLogs} />
            </div>
          </div>
        </div>
      </main>
    </div>
  );
}

export default App;
