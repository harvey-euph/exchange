import React, { useCallback } from 'react';

interface NumericInputProps {
  value: string;
  onChange: (v: string) => void;
  placeholder?: string;
  className?: string;
  step?: number;
  style?: React.CSSProperties;
  onKeyDown?: (e: React.KeyboardEvent<HTMLInputElement>) => void;
  onBlur?: (e: React.FocusEvent<HTMLInputElement>) => void;
  disabled?: boolean;
  allowDecimal?: boolean;
}

export const NumericInput: React.FC<NumericInputProps> = ({
  value, onChange, placeholder, className, step = 1, style, onKeyDown, onBlur, disabled, allowDecimal
}) => {
  const updateValue = useCallback((delta: number) => {
    if (disabled) return;
    try {
      if (allowDecimal) {
        const current = parseFloat(value || '0');
        const next = current + delta;
        // Derive decimal places from the step so e.g. step=0.25 → always 2 d.p.
        const stepStr = step.toString();
        const dotIdx = stepStr.indexOf('.');
        const decimalPlaces = dotIdx >= 0 ? stepStr.length - dotIdx - 1 : 0;
        onChange(next >= 0 ? next.toFixed(decimalPlaces) : (0).toFixed(decimalPlaces));
      } else {
        const current = BigInt(value || '0');
        const next = current + BigInt(Math.round(delta));
        onChange(next >= 0n ? next.toString() : '0');
      }
    } catch (e) {
      onChange('0');
    }
  }, [value, onChange, disabled, allowDecimal, step]);

  const handleInternalKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (disabled) return;
    if (e.key === 'ArrowUp') {
      e.preventDefault();
      updateValue(step);
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      updateValue(-step);
    }
    onKeyDown?.(e);
  };

  const onWheel = (e: React.WheelEvent) => {
    if (disabled) return;
    e.preventDefault();
    if (e.deltaY < 0) {
      updateValue(step);
    } else {
      updateValue(-step);
    }
  };

  return (
    <input
      type="text"
      value={value}
      onChange={e => {
        if (disabled) return;
        let val = e.target.value;
        if (allowDecimal) {
          val = val.replace(/[^0-9.]/g, '');
          const parts = val.split('.');
          if (parts.length > 2) {
            val = parts[0] + '.' + parts.slice(1).join('');
          }
        } else {
          val = val.replace(/[^0-9]/g, '');
        }
        onChange(val);
      }}
      onKeyDown={handleInternalKeyDown}
      onBlur={onBlur}
      onWheel={onWheel}
      placeholder={placeholder}
      className={`modern-input ${className || ''}`}
      style={{ ...style, opacity: disabled ? 0.6 : 1 }}
      disabled={disabled}
    />
  );
};
