# Current SIM state — Nov12

User has removed the SIM for now. Continue modem boot/PCIe diagnostics without
SIM. Jio India LTE/VoLTE remains the later target; no SIM detection or network
registration claim is possible in this state. Ask for reinsertion only when
SIM/network testing is ready.

Earlier carrier investigation below:

# Target carrier: Jio India, LTE and VoLTE

User confirmed on 2026-11-11 that a physical Jio SIM is now inserted in the S20.
This is user-provided state; our modem backend has not detected/read the SIM yet.
LTE registration, internet access and calls remain unavailable.

## Data and voice are separate acceptance paths

For public internet, Jio documents **JioNet** as its data APN. Stock iOS normally
configures it automatically, but this port lacks a working telephony backend;
we must implement the Samsung data session and verify actual provisioning.
[Jio APN instructions](https://www.jio.com/help/faq/mobile/services/high-speed-internet/how-do-i-set-the-apn-on-my-smartphone-to-jionet/).

For the requested VoLTE target, internet connectivity alone will not prove
voice readiness. Determine the Samsung AP/CP split for IMS, implement the
required provisioning/registration and call signaling, then validate two-way
audio through the modem/ABOX path and native iOS Phone integration. Do not
assume the public internet APN or a working packet session supplies this stack.

Jio states that VoLTE voice/SMS can work while the user's mobile-data switch is
off. Consequently, the eventual Cellular Data toggle must control public
internet traffic without inadvertently disabling IMS voice service.
[Jio voice with data switched off](https://www.jio.com/help/faq/mobile/services/hd-voice/will-i-be-able-to-use-voice-services-if-i-switch-off-data-on-my-jio-sim/).

## Later validation

- Actual SIM-ready/PIN-required/absent state, with no guessed PIN attempts.
- Jio LTE registration, rejection/reconnect handling and real signal state.
- Bidirectional public internet with Wi-Fi excluded and USB management retained.
- IMS registration and independent VoLTE call readiness.
- Incoming/outgoing call state, hangup/ringing, two-way audio and routing.
- Public data off while voice remains usable; idle/wake/reconnect behavior.

Do not print or commit IMSI, ICCID, IMEI, authentication material or raw private
IMS provisioning. No destination for a test call/SMS has been provided; obtain
one when that implementation is ready, rather than dialing an arbitrary number.

Implementation flow: [cellular-flow.md](flow.md).
