import React from 'react';
import { Side } from '../fbs/exchange/side';
import { NumericInput } from './NumericInput';
import { formatPrice } from '../types';

interface OrderEntryProps {
  // Login props
  isLoggedIn: boolean;
  clientId: string;
  setClientId: (v: string) => void;
  onLogin: () => void;
  
  // Order props
  price: string;
  quantity: string;
  side: Side;
  peggedLevel: number | null;
  setPrice: (v: string) => void;
  setQuantity: (v: string) => void;
  setSide: (s: Side) => void;
  setPeggedLevel: (p: number | null) => void;
  onSendOrder: (side: Side) => void;
  
  cash?: bigint;
  disabled?: boolean;
  priceExp?: number;
  priceMinStep?: bigint;
}

export const OrderEntry: React.FC<OrderEntryProps> = ({
  isLoggedIn, clientId, setClientId, onLogin,
  price, quantity, side, peggedLevel, 
  setPrice, setQuantity, setSide, setPeggedLevel,
  onSendOrder, cash, disabled, priceExp, priceMinStep
}) => {

  // Convert raw mantissa step → display-unit step (e.g. step=25, exp=-2 → 0.25)
  const priceDisplayStep = (() => {
    if (priceMinStep === undefined || priceMinStep <= 0n) return 1;
    if (priceExp === undefined || priceExp >= 0) return Number(priceMinStep);
    return Number(priceMinStep) / Math.pow(10, -priceExp);
  })();

  const handlePriceChange = (v: string) => {
    setPrice(v);
    setPeggedLevel(null);
  };

  const handleQtyChange = (v: string) => {
    setQuantity(v);
  };

  const getPegDisplay = () => {
    if (peggedLevel === null) return '';
    return `PEG TO ${side === Side.Buy ? 'BID' : 'ASK'} ${peggedLevel}`;
  };

  if (!isLoggedIn) {
    return (
      <div className="modern-card" style={{ padding: '12px', marginBottom: '16px' }}>
        <div className="block-header">
          <h2 className="block-title">Client Login</h2>
        </div>
        <div style={{ display: 'flex', flexDirection: 'column', gap: '12px' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
            <div style={{ width: '60px', fontSize: '12px', color: 'var(--text-secondary)', fontWeight: 500 }}>Client ID</div>
            <input 
              type="text" 
              className="modern-input"
              value={clientId} 
              onChange={(e) => setClientId(e.target.value)} 
              style={{ flex: 1, height: '32px' }} 
            />
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
            <div style={{ width: '60px', fontSize: '12px', color: 'var(--text-secondary)', fontWeight: 500 }}>Password</div>
            <input 
              type="password" 
              className="modern-input"
              value="********" 
              disabled
              style={{ flex: 1, height: '32px', opacity: 0.5 }} 
            />
          </div>
          <div style={{ marginTop: '4px' }}>
            <button 
              className="modern-button btn-primary" 
              onClick={onLogin}
              style={{ width: '100%', height: '36px', fontSize: '13px', fontWeight: 700 }}
            >
              LOGIN
            </button>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="modern-card" style={{ padding: '12px', marginBottom: '16px' }}>
      <div className="block-header">
        <h2 className="block-title">Order Entry</h2>
        {cash !== undefined && (
          <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <span style={{ fontSize: '11px', color: 'var(--text-secondary)' }}>Cash:</span>
            <span style={{ fontSize: '12px', color: 'var(--accent-blue)', fontWeight: 600, fontFamily: 'var(--font-mono)' }}>{formatPrice(cash, priceExp)}</span>
          </div>
        )}
      </div>
      <div style={{ display: 'flex', flexDirection: 'column', gap: '12px' }}>
        {/* Side Selection */}
        <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
          <div style={{ width: '60px', fontSize: '12px', color: 'var(--text-secondary)', fontWeight: 500 }}>Side</div>
          <div style={{ display: 'flex', gap: '16px', fontSize: '13px' }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: '6px', cursor: 'pointer' }}>
              <input 
                type="radio" 
                name="orderSide" 
                checked={side === Side.Buy} 
                onChange={() => { setSide(Side.Buy); setPeggedLevel(null); }}
                disabled={disabled}
              />
              Buy
            </label>
            <label style={{ display: 'flex', alignItems: 'center', gap: '6px', cursor: 'pointer' }}>
              <input 
                type="radio" 
                name="orderSide" 
                checked={side === Side.Sell} 
                onChange={() => { setSide(Side.Sell); setPeggedLevel(null); }}
                disabled={disabled}
              />
              Sell
            </label>
          </div>
        </div>

        {/* Price Input */}
        <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
          <div style={{ width: '60px', fontSize: '12px', color: 'var(--text-secondary)', fontWeight: 500 }}>Price</div>
          <NumericInput 
            value={peggedLevel !== null ? getPegDisplay() : price} 
            onChange={handlePriceChange} 
            step={priceDisplayStep}
            style={{ 
              flex: 1, 
              height: '32px', 
              boxSizing: 'border-box',
              color: peggedLevel !== null ? 'var(--accent-blue)' : '#fff',
              fontFamily: 'var(--font-mono)'
            }} 
            disabled={disabled}
            allowDecimal
          />
        </div>

        {/* Quantity Input */}
        <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
          <div style={{ width: '60px', fontSize: '12px', color: 'var(--text-secondary)', fontWeight: 500 }}>Qty</div>
          <NumericInput 
            value={quantity} 
            onChange={handleQtyChange} 
            style={{ 
              flex: 1, 
              height: '32px', 
              boxSizing: 'border-box',
              color: '#fff',
              fontFamily: 'var(--font-mono)'
            }} 
            disabled={disabled}
          />
        </div>

        {/* Send Button */}
        <div style={{ marginTop: '4px' }}>
          <button 
            className={`modern-button ${side === Side.Buy ? 'btn-buy' : 'btn-sell'}`} 
            onClick={() => onSendOrder(side)} 
            disabled={disabled}
            style={{ width: '100%', height: '36px', fontSize: '13px', fontWeight: 700 }}
          >
            SEND ORDER
          </button>
        </div>
      </div>
    </div>
  );
};
