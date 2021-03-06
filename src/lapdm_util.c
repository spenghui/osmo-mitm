#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/rsl.h>
#include <osmocom/gsm/lapd_core.h>
#include <osmocom/gsm/lapdm.h>
#include <errno.h>
#include <string.h>
#include <mitm/lapdm_util.h>

/* pull the layer 2 header and parse it to a lapdm context */
int pull_lapd_ctx(struct msgb *msg,
	uint8_t chan_nr, uint8_t link_id, enum lapdm_mode mode, struct lapdm_msg_ctx *mctx, struct lapd_msg_ctx *lctx)
{
	uint8_t cbits = chan_nr >> 3;
	int n201;

	/* when we reach here, we have a msgb with l2h pointing to the raw
	 * 23byte mac block. The l1h has already been purged. */

	mctx->chan_nr = chan_nr;
	mctx->link_id = link_id;

	/* check for L1 chan_nr/link_id and determine LAPDm hdr format */
	if (cbits == 0x10 || cbits == 0x12) {
		/* Format Bbis is used on BCCH and CCCH(PCH, NCH and AGCH) */
		mctx->lapdm_fmt = LAPDm_FMT_Bbis;
		n201 = N201_Bbis;
	} else {
		if (mctx->link_id & 0x40) {
			/* It was received from network on SACCH */

			/* If UI on SACCH sent by BTS, lapdm_fmt must be B4 */
			if (mode == LAPDM_MODE_MS
			 && LAPDm_CTRL_is_U(msg->l2h[3])
			 && LAPDm_CTRL_U_BITS(msg->l2h[3]) == 0) {
				n201 = N201_B4;
				mctx->lapdm_fmt = LAPDm_FMT_B4;
			} else {
				n201 = N201_AB_SACCH;
				mctx->lapdm_fmt = LAPDm_FMT_B;
			}
			/* SACCH frames have a two-byte L1 header that
			 * OsmocomBB L1 doesn't strip */
			mctx->tx_power_ind = msg->l2h[0] & 0x1f;
			mctx->ta_ind = msg->l2h[1];
			msg->l2h = msgb_pull(msg, 2);
		} else {
			n201 = N201_AB_SDCCH;
			mctx->lapdm_fmt = LAPDm_FMT_B;
		}
	}

	switch (mctx->lapdm_fmt) {
	case LAPDm_FMT_A:
	case LAPDm_FMT_B:
	case LAPDm_FMT_B4:
		// We are not interested in the actual datalink here
		lctx->dl = NULL;
		/* obtain SAPI from address field */
		mctx->link_id |= LAPDm_ADDR_SAPI(msg->l2h[0]);
		/* G.2.3 EA bit set to "0" is not allowed in GSM */
		if (!LAPDm_ADDR_EA(msg->l2h[0])) {
			return -EINVAL;
		}
		/* adress field */
		lctx->lpd = LAPDm_ADDR_LPD(msg->l2h[0]);
		lctx->sapi = LAPDm_ADDR_SAPI(msg->l2h[0]);
		lctx->cr = LAPDm_ADDR_CR(msg->l2h[0]);
		/* command field */
		if (LAPDm_CTRL_is_I(msg->l2h[1])) {
			lctx->format = LAPD_FORM_I;
			lctx->n_send = LAPDm_CTRL_I_Ns(msg->l2h[1]);
			lctx->n_recv = LAPDm_CTRL_Nr(msg->l2h[1]);
		} else if (LAPDm_CTRL_is_S(msg->l2h[1])) {
			lctx->format = LAPD_FORM_S;
			lctx->n_recv = LAPDm_CTRL_Nr(msg->l2h[1]);
			lctx->s_u = LAPDm_CTRL_S_BITS(msg->l2h[1]);
		} else if (LAPDm_CTRL_is_U(msg->l2h[1])) {
			lctx->format = LAPD_FORM_U;
			lctx->s_u = LAPDm_CTRL_U_BITS(msg->l2h[1]);
		} else
			lctx->format = LAPD_FORM_UKN;
		lctx->p_f = LAPDm_CTRL_PF_BIT(msg->l2h[1]);
		if (lctx->sapi != LAPDm_SAPI_NORMAL
		 && lctx->sapi != LAPDm_SAPI_SMS
		 && lctx->format == LAPD_FORM_U
		 && lctx->s_u == LAPDm_U_UI) {
			/* 5.3.3 UI frames with invalid SAPI values shall be
			 * discarded
			 */
			return -EINVAL;
		}
		if (mctx->lapdm_fmt == LAPDm_FMT_B4) {
			lctx->n201 = n201;
			lctx->length = n201;
			lctx->more = 0;
			msg->l3h = msg->l2h + 2;
			msgb_pull_to_l3(msg);
		} else {
			/* length field */
			if (!(msg->l2h[2] & LAPDm_EL)) {
				/* G.4.1 If the EL bit is set to "0", an
				 * MDL-ERROR-INDICATION primitive with cause
				 * "frame not implemented" is sent to the
				 * mobile management entity. */
				return -EINVAL;
			}
			lctx->n201 = n201;
			lctx->length = msg->l2h[2] >> 2;
			lctx->more = !!(msg->l2h[2] & LAPDm_MORE);
			msg->l3h = msg->l2h + 3;
			msgb_pull_to_l3(msg);
		}
		break;
	case LAPDm_FMT_Bter:
		/* FIXME not implemented */
		return -EINVAL;
	case LAPDm_FMT_Bbis:
		/* directly pass up to layer3, we have no lapdm header in this case */
		msg->l3h = msg->l2h;
		msgb_pull_to_l3(msg);
		return -EINVAL;
	default:
		msgb_free(msg);
	}

	return 0;
}

void lapdm_set_length (uint8_t *l2_hdr, uint8_t len, uint8_t more_seg, uint8_t final_oct) {
	uint8_t len_field = final_oct;
	len_field |= more_seg << 1;
	len_field |= len << 2;
	l2_hdr[2] = len_field;
}
