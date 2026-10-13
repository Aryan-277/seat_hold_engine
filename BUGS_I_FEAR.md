# BUGS_I_FEAR.md

## Bug class my tests would NOT catch: engine state and the payment system disagreeing
My tests check the engine's own invariants. But the customer is charged by a payment provider and calls
confirm() afterwards. Because of the R3 grace rule, confirm() can return HOLD_LOST after the card was already
charged: the engine is consistent, yet the customer paid for a seat they don't own. The reverse also exists:
confirm() succeeds but the charge fails or its response is lost, leaving a SOLD seat with no payment.

How I'd catch it in production:
1. Reconciliation job every few minutes joining captured payments with SOLD seats by hold_id. Alert on a payment
   without a SOLD seat (auto-refund) and a SOLD seat without a payment (release the seat).
2. Alert when confirm() returns HOLD_LOST or HOLD_RELEASED while a payment authorisation exists.
3. Structural fix: authorise first, confirm() second, capture only after confirm() succeeds.

## Smaller fears
- Real clocks are not monotonic (NTP steps, skew between nodes). Use a monotonic source and alert on negative deltas.
- A process restart loses all holds and sales (in-memory). Use a write-ahead log or database plus the reconciliation job.