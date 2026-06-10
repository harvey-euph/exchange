import { Side } from './fbs/exchange/side';

export interface OrderData {
  orderId: string;
  symbolId: number;
  side: Side;
  p: bigint;
  q: bigint;
  filled: bigint;
}

export interface PositionLot {
  price: bigint;
  quantity: bigint;
  timestamp: number;
  orderId: string;
}

export interface SymbolPosition {
  symbolId: number;
  side: Side;
  lots: PositionLot[];
  totalQuantity: bigint;
  averagePrice: bigint;
  realizedPnL: bigint;
}

export interface ConnectedState {
  mgmt: boolean;
  mgmtReady: boolean;
  l2: boolean;
}

export interface SymbolInfoData {
  symbolId: number;
  name: string;
  priceExp: number;
  priceMinStep: bigint;
  priceMin: bigint;
  priceMax: bigint;
}

export function formatPrice(price: bigint, priceExp: number | undefined): string {
  if (priceExp === undefined) return price.toString();
  if (priceExp >= 0) {
    return (price * BigInt(10 ** priceExp)).toString();
  }
  const expLength = -priceExp;
  const factor = BigInt(10 ** expLength);
  const isNegative = price < 0n;
  const absPrice = isNegative ? -price : price;

  const integerPart = absPrice / factor;
  const fractionalPart = absPrice % factor;
  // Always pad to full expLength so e.g. 80000 → "80000.00"
  const fracStr = fractionalPart.toString().padStart(expLength, '0');

  return `${isNegative ? '-' : ''}${integerPart}.${fracStr}`;
}

/**
 * Returns the number of decimal places implied by a priceExp value.
 * e.g. priceExp=-2 → 2,  priceExp=0 → 0
 */
export function priceDecimalPlaces(priceExp: number | undefined): number {
  if (priceExp === undefined || priceExp >= 0) return 0;
  return -priceExp;
}

export function parsePrice(priceStr: string, priceExp: number | undefined): bigint {
  if (priceExp === undefined || priceExp >= 0) {
    const clean = priceStr.replace(/[^0-9]/g, '');
    return BigInt(clean || '0');
  }
  
  const parts = priceStr.split('.');
  const integerPart = BigInt(parts[0].replace(/[^0-9]/g, '') || '0');
  let fractionStr = parts[1] || '';
  const expLength = -priceExp;
  if (fractionStr.length > expLength) {
    fractionStr = fractionStr.substring(0, expLength);
  } else {
    fractionStr = fractionStr.padEnd(expLength, '0');
  }
  const fractionalPart = BigInt(fractionStr.replace(/[^0-9]/g, '') || '0');
  
  return integerPart * BigInt(10 ** expLength) + fractionalPart;
}

export function getPriceExpForSymbol(symbolId: number): number {
  if (symbolId === 3) return -3;
  return -2; // BTC and ETH are -2, SOL is -3
}
