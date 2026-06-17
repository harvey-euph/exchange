# Latency Comparison Report

## Metrics Table

| ExecType | Stage | Baseline P50 | Candidate P50 | P50 Δ | Baseline P99 | Candidate P99 | P99 Δ | p-value | Cliff Delta | Result |
| -------- | ----- | ------------ | ------------- | ----- | ------------ | ------------- | ----- | ------- | ----------- | ------ |
| Cancel | 0-1 lat | 7 us | 6.8 us | +3.4% | 17.9 us | 17.4 us | +2.7% | <0.001 | -0.06 | No Change |
| Cancel | 1-2 lat | 2.3 us | 2.1 us | +5.7% | 4 us | 3.8 us | +4.5% | <0.001 | -0.12 | No Change |
| Cancel | 2-3 lat | 2.2 us | 2.2 us | -0.9% | 6.6 us | 6.7 us | -1.2% | <0.001 | -0.01 | No Change |
| Cancel | 3-4 lat | 2.1 us | 2.1 us | -0.5% | 4.4 us | 4.2 us | +3.4% | <0.001 | -0.01 | No Change |
| Cancel | 4-5 lat | 6 us | 5 us | +16.4% | 19.3 us | 16.4 us | +15.4% | <0.001 | -0.11 | Improved |
| Cancel | 5-6 lat | 2.2 us | 2.1 us | +6.7% | 3.6 us | 3.4 us | +4.4% | <0.001 | -0.18 | No Change |
| Cancel | 6-7 lat | 5.7 us | 5 us | +11.7% | 13.7 us | 12.7 us | +6.6% | <0.001 | -0.16 | No Change |
| Cancel | 7-8 lat | 15.2 us | 14.2 us | +6.7% | 34.1 us | 29.5 us | +13.5% | <0.001 | -0.29 | Improved |
| Cancel | Total | 47.1 us | 42.1 us | +10.5% | 77 us | 72.2 us | +6.2% | <0.001 | -0.18 | No Change |
| Modify-Long | 0-1 lat | 7 us | 6.8 us | +3.3% | 18 us | 17.5 us | +3.1% | <0.001 | -0.06 | No Change |
| Modify-Long | 1-2 lat | 2.3 us | 2.2 us | +6.1% | 4 us | 3.9 us | +2.7% | <0.001 | -0.11 | No Change |
| Modify-Long | 2-3 lat | 2.2 us | 2.2 us | -0.4% | 6.8 us | 6.5 us | +5% | <0.001 | -0.01 | No Change |
| Modify-Long | 3-4 lat | 2.6 us | 2.7 us | -2.7% | 11.8 us | 11.8 us | -0.2% | 1.000 | 0.02 | No Change |
| Modify-Long | 4-5 lat | 5.8 us | 4.9 us | +15.5% | 163.6 us | 138.1 us | +15.6% | <0.001 | -0.09 | Improved |
| Modify-Long | 5-6 lat | 2.3 us | 2.1 us | +7.5% | 3.7 us | 3.6 us | +4.3% | <0.001 | -0.18 | No Change |
| Modify-Long | 6-7 lat | 5.3 us | 4.6 us | +12.5% | 12.9 us | 11.4 us | +11.6% | <0.001 | -0.15 | Improved |
| Modify-Long | 7-8 lat | 15 us | 14.1 us | +6.4% | 33.8 us | 29.3 us | +13.2% | <0.001 | -0.27 | Improved |
| Modify-Long | Total | 47.1 us | 43.4 us | +7.8% | 206.2 us | 181.2 us | +12.2% | <0.001 | -0.16 | Improved |
| Modify-Short | 0-1 lat | 7 us | 6.8 us | +3.8% | 18.7 us | 17.6 us | +5.7% | <0.001 | -0.06 | No Change |
| Modify-Short | 1-2 lat | 2.3 us | 2.2 us | +6.5% | 4.1 us | 4 us | +2% | <0.001 | -0.12 | No Change |
| Modify-Short | 2-3 lat | 2.2 us | 2.2 us | -0.4% | 7.4 us | 6.7 us | +9.8% | <0.001 | -0.01 | No Change |
| Modify-Short | 3-4 lat | 1.9 us | 1.9 us | -0.5% | 4 us | 3.9 us | +3% | <0.001 | -0.01 | No Change |
| Modify-Short | 4-5 lat | 6.2 us | 5.2 us | +16.5% | 19.2 us | 16.9 us | +12.3% | <0.001 | -0.11 | Improved |
| Modify-Short | 5-6 lat | 2.3 us | 2.1 us | +7% | 3.7 us | 3.6 us | +3.2% | <0.001 | -0.18 | No Change |
| Modify-Short | 6-7 lat | 5.4 us | 4.7 us | +13% | 12.8 us | 11.6 us | +9.6% | <0.001 | -0.16 | No Change |
| Modify-Short | 7-8 lat | 15.1 us | 14.2 us | +6.5% | 34.3 us | 29.6 us | +13.8% | <0.001 | -0.29 | Improved |
| Modify-Short | Total | 46.8 us | 41.7 us | +10.9% | 76.8 us | 72 us | +6.3% | <0.001 | -0.18 | No Change |
| New | 0-1 lat | 7 us | 6.8 us | +3.4% | 18.1 us | 17.4 us | +3.6% | <0.001 | -0.05 | No Change |
| New | 1-2 lat | 2.3 us | 2.1 us | +5.7% | 4 us | 3.8 us | +5% | <0.001 | -0.12 | No Change |
| New | 2-3 lat | 2.2 us | 2.2 us | -0.9% | 7.2 us | 7.1 us | +1.5% | <0.001 | -0.01 | No Change |
| New | 3-4 lat | 1.2 us | 1.2 us | +0.7% | 3.2 us | 3.1 us | +4% | <0.001 | -0.02 | No Change |
| New | 4-5 lat | 6.9 us | 5.8 us | +15% | 21.1 us | 18.4 us | +12.8% | <0.001 | -0.11 | Improved |
| New | 5-6 lat | 2.2 us | 2.1 us | +6.7% | 3.7 us | 3.5 us | +4.6% | <0.001 | -0.18 | No Change |
| New | 6-7 lat | 4.9 us | 4.3 us | +12.5% | 15.9 us | 14.7 us | +7.1% | <0.001 | -0.14 | No Change |
| New | 7-8 lat | 15.1 us | 14.2 us | +6.3% | 34.3 us | 29.7 us | +13.3% | <0.001 | -0.28 | Improved |
| New | Total | 46.3 us | 41.4 us | +10.5% | 77.1 us | 72 us | +6.6% | <0.001 | -0.17 | No Change |
| Reject | 0-1 lat | 7 us | 6.8 us | +3.4% | 17.7 us | 17.3 us | +2.3% | <0.001 | -0.05 | No Change |
| Reject | 1-2 lat | 2.3 us | 2.2 us | +6.1% | 4 us | 3.9 us | +3% | <0.001 | -0.11 | No Change |
| Reject | 2-3 lat | 2.2 us | 2.2 us | -0.9% | 6.8 us | 6.5 us | +5.2% | <0.001 | -0.01 | No Change |
| Reject | 3-4 lat | 1.3 us | 1.2 us | +0.9% | 2.4 us | 2.4 us | +1.7% | <0.001 | -0.02 | No Change |
| Reject | 4-5 lat | 6.8 us | 5.9 us | +13.9% | 21.2 us | 17.8 us | +16.3% | <0.001 | -0.12 | Improved |
| Reject | 5-6 lat | 2.3 us | 2.1 us | +6.9% | 3.6 us | 3.5 us | +3.3% | <0.001 | -0.18 | No Change |
| Reject | 6-7 lat | 4.1 us | 3.8 us | +7.5% | 10.1 us | 8.5 us | +16% | <0.001 | -0.12 | Improved |
| Reject | 7-8 lat | 14.3 us | 13.9 us | +2.9% | 31.1 us | 28.4 us | +8.8% | <0.001 | -0.17 | No Change |
| Reject | Total | 41.4 us | 39.7 us | +4.1% | 72.2 us | 69 us | +4.4% | <0.001 | -0.11 | No Change |

## Final Summary

Overall:  
  
Total P99:  
+10%  
  
Improved:  
12 stages  
  
Regressed:  
0 stages  
  
Largest improvement:  
Reject / 4-5 lat (+16.3%)  
  
Largest regression:  
Cancel / 2-3 lat (-1.2%)  
  
Release verdict: