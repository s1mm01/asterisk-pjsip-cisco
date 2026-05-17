/*
 * Asterisk -- An open source telephony toolkit.
 *
 * cisco_orig_host: outbound-request URI rewrite for NAT'd contacts.
 *
 * When res_pjsip_nat sees a REGISTER from a phone whose Contact URI
 * host doesn't match the source IP (i.e. a NAT'd phone advertising
 * its LAN address), it rewrites the registered Contact's host:port
 * to the public NAT mapping and saves the original as the
 * `x-ast-orig-host=LAN:port` URI parameter (res_pjsip_nat.c:43-67).
 *
 * Cisco Enterprise firmware on the WAN side (verified against
 * CP8861/14.1.1 and CP7975G/9.4.2) rejects unsolicited NOTIFYs and
 * REFERs with 400 Bad Request when the Request-URI and To-URI use
 * the public NAT mapping rather than the host the phone last
 * advertised about itself. Same firmware on a LAN contact accepts
 * the same request bytes.
 *
 * Bodies live in res/res/cisco_endpoint/orig_host.c, compiled into
 * res_pjsip_cisco_endpoint.so. Activated by res_pjsip_cisco_endpoint's
 * load_module; applies to every outbound SIP request that carries an
 * x-ast-orig-host URI parameter on its Request-URI (so it's a no-op
 * for LAN-registered contacts, trunk targets, etc.). Consumers don't
 * need to opt in — sending an out-of-dialog request to a NAT'd
 * Cisco-phone contact is enough.
 */

#ifndef _RES_PJSIP_CISCO_ORIG_HOST_H
#define _RES_PJSIP_CISCO_ORIG_HOST_H

/*!
 * \brief Register the global on_tx_request hook that rewrites RURI
 *        and To-URI host:port from the x-ast-orig-host parameter.
 *
 * Idempotent: safe to call once per module load. Returns 0 on success,
 * -1 if pjsip refuses the registration.
 */
int cisco_orig_host_register(void);

/*!
 * \brief Unregister the hook. Symmetric with the registrar above.
 */
void cisco_orig_host_unregister(void);

#endif /* _RES_PJSIP_CISCO_ORIG_HOST_H */
