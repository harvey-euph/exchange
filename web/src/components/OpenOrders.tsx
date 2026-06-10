import React, { useState, useEffect, useMemo } from 'react';
import { Side } from '../fbs/exchange/side';
import type { OrderData, SymbolInfoData } from '../types';
import { NumericInput } from './NumericInput';
import { formatPrice, getPriceExpForSymbol } from '../types';

interface OpenOrdersProps {
  orders: OrderData[];
  onModify: (order: OrderData, newPrice: string, newQty: string) => void;
  onCancel: (order: OrderData) => void;
  currentSymbolId?: string;
  noWrapper?: boolean;
  expandedSymbols: Set<number>;
  onToggleSymbol: (sid: number) => void;
  symbolInfo?: SymbolInfoData | null;
}

export const OpenOrders: React.FC<OpenOrdersProps> = ({
  orders, onModify, onCancel, currentSymbolId, noWrapper,
  expandedSymbols, onToggleSymbol, symbolInfo
}) => {
  const [editValues, setEditValues] = useState<Record<string, { p: string, q: string }>>({});
  const lastOrdersRef = React.useRef<Map<string, { p: string, q: string }>>(new Map());

  // Group orders by symbolId
  const ordersBySymbol = useMemo(() => {
    const groups: Record<number, OrderData[]> = {};
    orders.forEach(o => {
      if (!groups[o.symbolId]) groups[o.symbolId] = [];
      groups[o.symbolId].push(o);
    });
    return groups;
  }, [orders]);

  const sortedSymbols = useMemo(() => {
    return Object.keys(ordersBySymbol).map(Number).sort((a, b) => a - b);
  }, [ordersBySymbol]);

  // Helper: prefer live symbolInfo's priceExp over the hardcoded map
  const getExp = (sid: number): number => {
    if (symbolInfo && symbolInfo.symbolId === sid) return symbolInfo.priceExp;
    return getPriceExpForSymbol(sid);
  };

  // Initialize or update edit values when orders change
  useEffect(() => {
    setEditValues(prev => {
      const next = { ...prev };
      orders.forEach(o => {
        const orderId = o.orderId;
        const exp = getExp(o.symbolId);
        const pStr = formatPrice(o.p, exp);
        const qStr = o.q.toString();
        const last = lastOrdersRef.current.get(orderId);

        if (!next[orderId] || (last && (last.p !== pStr || last.q !== qStr))) {
          next[orderId] = { p: pStr, q: qStr };
        }
        lastOrdersRef.current.set(orderId, { p: pStr, q: qStr });
      });

      const currentIds = new Set(orders.map(o => o.orderId));
      Object.keys(next).forEach(id => {
        if (!currentIds.has(id)) {
          delete next[id];
          lastOrdersRef.current.delete(id);
        }
      });

      return next;
    });
  }, [orders, symbolInfo]);

  const handleUpdate = (orderId: string, field: 'p' | 'q', val: string) => {
    setEditValues(prev => ({
      ...prev,
      [orderId]: { ...prev[orderId], [field]: val }
    }));
  };

  const handleKeyDown = (e: React.KeyboardEvent, order: OrderData) => {
    if (e.key === 'Enter') {
      const vals = editValues[order.orderId];
      if (vals) {
        onModify(order, vals.p, vals.q);
      }
      handleRevert(order.orderId, order);
      (e.target as HTMLInputElement).blur();
    } else if (e.key === 'Escape') {
      handleRevert(order.orderId, order);
      (e.target as HTMLInputElement).blur();
    }
  };


  const handleRevert = (orderId: string, order: OrderData) => {
    const exp = getExp(order.symbolId);
    setEditValues(prev => ({
      ...prev,
      [orderId]: { p: formatPrice(order.p, exp), q: order.q.toString() }
    }));
  };

  // Cancel all orders for a symbol, optionally filtered by side
  const handleCancelSymbol = (e: React.MouseEvent, sid: number, side?: Side) => {
    e.stopPropagation(); // don't toggle expand/collapse
    const targets = ordersBySymbol[sid] ?? [];
    targets
      .filter(o => side === undefined || o.side === side)
      .forEach(o => onCancel(o));
  };

  const content = (
    <>
      <div className="table-container custom-scroll">
        {sortedSymbols.length === 0 ? (
          <div style={{ textAlign: 'center', padding: '40px', color: 'var(--text-secondary)', fontSize: '13px' }}>
            No open orders
          </div>
        ) : (
          <table className="modern-table" style={{ tableLayout: 'fixed' }}>
            <thead>
              <tr>
                <th style={{ width: '70px', textAlign: 'right' }}>Order ID</th>
                <th style={{ width: '30px', textAlign: 'right' }}>Side</th>
                <th style={{ width: '65px', textAlign: 'right' }}>Price</th>
                <th style={{ width: '65px', textAlign: 'right' }}>Qty</th>
                <th style={{ width: '45px', textAlign: 'right' }}>Fill</th>
                <th style={{ width: '55px', textAlign: 'right' }}></th>
              </tr>
            </thead>
            <tbody>
              {sortedSymbols.map(sid => (
                <React.Fragment key={sid}>
                  <tr className="symbol-group-header" onClick={() => onToggleSymbol(sid)}>
                    <td colSpan={6}>
                      <div className="symbol-group-title" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                        <div style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
                          <span className={`expand-icon ${expandedSymbols.has(sid) ? 'expanded' : ''}`}>▼</span>
                          <span>Symbol {sid}</span>
                          <span style={{ color: 'var(--text-secondary)', fontSize: '10px' }}>
                            ({ordersBySymbol[sid].length})
                          </span>
                        </div>
                        <div style={{ display: 'flex', gap: '4px' }} onClick={e => e.stopPropagation()}>
                          <button
                            className="modern-button"
                            title="Cancel all Buy orders"
                            onClick={e => handleCancelSymbol(e, sid, Side.Buy)}
                            style={{
                              padding: '3px 10px', fontSize: '11px', height: '24px',
                              background: 'rgba(52,211,153,0.15)',
                              color: 'var(--accent-green)',
                              border: '1px solid rgba(52,211,153,0.35)',
                            }}
                          >
                            ✕ Buy
                          </button>
                          <button
                            className="modern-button"
                            title="Cancel all Sell orders"
                            onClick={e => handleCancelSymbol(e, sid, Side.Sell)}
                            style={{
                              padding: '3px 10px', fontSize: '11px', height: '24px',
                              background: 'rgba(248,113,113,0.15)',
                              color: 'var(--accent-red)',
                              border: '1px solid rgba(248,113,113,0.35)',
                            }}
                          >
                            ✕ Sell
                          </button>
                          <button
                            className="modern-button"
                            title="Cancel all orders"
                            onClick={e => handleCancelSymbol(e, sid)}
                            style={{
                              padding: '3px 10px', fontSize: '11px', height: '24px',
                              background: 'rgba(251,191,36,0.15)',
                              color: '#fbbf24',
                              border: '1px solid rgba(251,191,36,0.35)',
                            }}
                          >
                            ✕ All
                          </button>
                        </div>
                      </div>
                    </td>
                  </tr>
                  {expandedSymbols.has(sid) && ordersBySymbol[sid].map((o, index) => {
                    const exp = getExp(o.symbolId);
                    const formattedPriceVal = formatPrice(o.p, exp);
                    const vals = editValues[o.orderId] || { p: formattedPriceVal, q: o.q.toString() };
                    const isModified = vals.p !== formattedPriceVal || vals.q !== o.q.toString();
                    const displayId = `${o.orderId}`;
                    
                    return (
                      <tr 
                        key={o.orderId} 
                        style={{ 
                          borderBottom: '1px solid var(--border-color)',
                          backgroundColor: index % 2 === 0 ? 'transparent' : 'rgba(255,255,255,0.02)'
                        }}
                      >
                        <td style={{ textAlign: 'right', color: 'var(--text-secondary)', fontSize: '10px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={o.orderId}>{displayId}</td>
                        <td style={{ 
                          textAlign: 'right',
                          color: o.side === Side.Buy ? 'var(--accent-green)' : o.side === Side.Sell ? 'var(--accent-red)' : 'var(--text-secondary)',
                          fontWeight: 600,
                          fontSize: '10px'
                        }}>
                          {Side[o.side]}
                        </td>
                        <td style={{ textAlign: 'right' }}>
                          <NumericInput 
                            className="editable-cell-input"
                            value={vals.p} 
                            onChange={(v) => handleUpdate(o.orderId, 'p', v)}
                            onKeyDown={(e) => handleKeyDown(e, o)}
                            onBlur={() => handleRevert(o.orderId, o)}
                            allowDecimal
                            step={(() => {
                              // Use symbolInfo step if available and matches this order's symbol
                              if (symbolInfo && symbolInfo.symbolId === o.symbolId && symbolInfo.priceMinStep > 0n) {
                                const exp = symbolInfo.priceExp;
                                if (exp >= 0) return Number(symbolInfo.priceMinStep);
                                return Number(symbolInfo.priceMinStep) / Math.pow(10, -exp);
                              }
                              return 1;
                            })()}
                            style={{ 
                              width: '70%', 
                              height: '22px', 
                              textAlign: 'right', 
                              fontSize: '11px',
                              border: isModified ? '1px solid var(--accent-blue)' : '1px solid transparent',
                              padding: '0 4px'
                            }} 
                          />
                        </td>
                        <td style={{ textAlign: 'right' }}>
                          <NumericInput 
                            className="editable-cell-input"
                            value={vals.q} 
                            onChange={(v) => handleUpdate(o.orderId, 'q', v)}
                            onKeyDown={(e) => handleKeyDown(e, o)}
                            onBlur={() => handleRevert(o.orderId, o)}
                            style={{ 
                              width: '70%', 
                              height: '22px', 
                              textAlign: 'right', 
                              fontSize: '11px',
                              border: isModified ? '1px solid var(--accent-blue)' : '1px solid transparent',
                              padding: '0 4px'
                            }} 
                          />
                        </td>
                        <td style={{ textAlign: 'right', color: 'var(--text-secondary)', fontSize: '11px' }}>{o.filled.toString()}</td>
                        <td style={{ textAlign: 'right' }}>
                          <button 
                            className="modern-button btn-sell" 
                            onClick={() => onCancel(o)}
                            style={{ padding: '2px 4px', fontSize: '10px', height: '22px', minWidth: '45px' }}
                          >
                            Cancel
                          </button>
                        </td>
                      </tr>
                    );
                  })}
                </React.Fragment>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  );

  if (noWrapper) return content;

  return (
    <div className="modern-card open-orders-section">
      {content}
    </div>
  );
};
