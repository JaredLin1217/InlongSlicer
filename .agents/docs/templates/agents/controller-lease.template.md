# Controller Lease

- Controller id:
- Lease id:
- Lease status:
- Acquired at:
- Last heartbeat:
- Expires at:
- Current task:
- Snapshot version reviewed:
- Released at:
- Takeover reason:

## Rules

- Only active lease holder writes agent-status.md or marks events processed.
- Renew before expiry while coordinating employees.
- Take over only after expiry, explicit release, or explicit user authorization.
- If lease state is ambiguous, do not write the shared status snapshot until an active controller is chosen.
