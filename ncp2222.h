/* ncp2222.h
 * Routines for NetWare Core Protocol
 * Gilbert Ramirez <gram@xiexie.org>
 *
 * $Id: ncp2222.h,v 1.2 2000/08/22 06:38:16 gram Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 2000 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/* Does NCP func require a subfunction code? */
static gboolean
ncp_requires_subfunc(guint8 func)
{
	const guint8 *ncp_func_requirement = ncp_func_requires_subfunc;

	while (*ncp_func_requirement != 0) {
		if (*ncp_func_requirement == func) {
			return TRUE;
		}
		ncp_func_requirement++;
	}
	return FALSE;
}

/* Return a ncp_record* based on func and possibly subfunc */
static const ncp_record *
ncp_record_find(guint8 func, guint8 subfunc)
{
	const ncp_record *ncp_rec = ncp_packets;

	while(ncp_rec->func != 0 || ncp_rec->subfunc != 0 ||
		ncp_rec->name != NULL ) {
		if (ncp_rec->func == func &&
			ncp_rec->subfunc == (subfunc & ncp_rec->submask)) {
			return ncp_rec;
		}
		ncp_rec++;
	}
	return NULL;
}

/* Run through the table of ptv_record's and add info to the tree */
static void
process_ptvc_record(ptvcursor_t *ptvc, const ptvc_record *rec)
{
	while(rec->hf_ptr != NULL) {
		ptvcursor_add(ptvc, *rec->hf_ptr, rec->length,
				rec->endianness);
		rec++;
	}
}


/* Given an error_equivalency table and a completion code, return
 * the string representing the error. */
static const char*
ncp_error_string(const error_equivalency *errors, guint8 completion_code)
{
	while (errors->ncp_error_index != -1) {
		if (errors->error_in_packet == completion_code) {
			return ncp_errors[errors->ncp_error_index];
		}
		errors++;
	}

	return "Unknown";
}

void
dissect_ncp_request(tvbuff_t *tvb, packet_info *pinfo,
		guint16 nw_connection, guint8 sequence,
		guint16 type, proto_tree *ncp_tree, proto_tree *tree)
{
	guint8			func, subfunc = 0;
	gboolean		requires_subfunc;
	const ncp_record	*ncp_rec;
	conversation_t		*conversation;
	ptvcursor_t		*ptvc = NULL;

	func = tvb_get_guint8(tvb, 6);

	requires_subfunc = ncp_requires_subfunc(func);
	if (requires_subfunc) {
		subfunc = tvb_get_guint8(tvb, 9);
	}

	ncp_rec = ncp_record_find(func, subfunc);

	if (check_col(pinfo->fd, COL_INFO)) {
		if (ncp_rec) {
			col_add_fstr(pinfo->fd, COL_INFO, "C %s", ncp_rec->name);
		}
		else {
			if (requires_subfunc) {
				col_add_fstr(pinfo->fd, COL_INFO,
						"C Unknown Function 0x%02X/0x%02x",
						func, subfunc);
			}
			else {
				col_add_fstr(pinfo->fd, COL_INFO,
						"C Unknown Function 0x%02x",
						func);
			}
		}
	}

	if (!pinfo->fd->flags.visited) {
		/* This is the first time we've looked at this packet.
		   Keep track of the address and connection whence the request
		   came, and the address and connection to which the request
		   is being sent, so that we can match up calls with replies.
		   (We don't include the sequence number, as we may want
		   to have all packets over the same connection treated
		   as being part of a single conversation so that we can
		   let the user select that conversation to be displayed.) */
		conversation = find_conversation(&pi.src, &pi.dst,
		    PT_NCP, nw_connection, nw_connection);
		if (conversation == NULL) {
			/* It's not part of any conversation - create a new one. */
			conversation = conversation_new(&pi.src, &pi.dst,
			    PT_NCP, nw_connection, nw_connection, NULL);
		}
		ncp_hash_insert(conversation, sequence, 0x2222, ncp_rec);
	}

	if (ncp_tree) {
		proto_tree_add_uint_format(ncp_tree, hf_ncp_func, tvb, 6, 1,
			func, "Function Code: 0x%02X (%s)",
			func, ncp_rec ? ncp_rec->name : "Unknown");

		if (requires_subfunc) {
			proto_tree_add_item(ncp_tree, hf_ncp_length, tvb, 7,
					2, FALSE);
			proto_tree_add_item(ncp_tree, hf_ncp_subfunc, tvb, 9,
					1, FALSE);
			ptvc = ptvcursor_new(ncp_tree, tvb, 10);
		}
		else {
			ptvc = ptvcursor_new(ncp_tree, tvb, 7);
		}

		/* The group is not part of the packet, but it's useful
		 * information to display anyway. */
		if (ncp_rec) {
			proto_tree_add_text(ncp_tree, tvb, 6, 1, "Group: %s",
					ncp_groups[ncp_rec->group]);
		}

		if (ncp_rec && ncp_rec->request_ptvc) {
			process_ptvc_record(ptvc, ncp_rec->request_ptvc);
		}
		ptvcursor_free(ptvc);
	}
}


void
dissect_ncp_reply(tvbuff_t *tvb, packet_info *pinfo,
	guint16 nw_connection, guint8 sequence,
	proto_tree *ncp_tree, proto_tree *tree) {

	conversation_t			*conversation;
	const ncp_record		*ncp_rec = NULL;
	guint16				ncp_type;
	gboolean			found_request = FALSE;
	guint8				completion_code;
	guint				length;
	ptvcursor_t			*ptvc = NULL;
	const char			*error_string;

	/* Find the conversation whence the request would have come. */
	conversation = find_conversation(&pi.src, &pi.dst,
		    PT_NCP, nw_connection, nw_connection);
	if (conversation != NULL) {
		/* find the record telling us the request made that caused
		   this reply */
		found_request = ncp_hash_lookup(conversation, sequence,
				&ncp_type, &ncp_rec);
	}
	/* else... we haven't seen an NCP Request for that conversation and sequence. */

	/* A completion code of 0 always means OK. Non-zero means failure,
	 * but each non-zero value has a different meaning. And the same value
	 * can have different meanings, depending on the ncp.func (and ncp.subfunc)
	 * value. */
	completion_code = tvb_get_guint8(tvb, 6);
	if (ncp_rec && ncp_rec->errors) {
		error_string = ncp_error_string(ncp_rec->errors, completion_code);
	}
	else if (completion_code == 0) {
		error_string = "OK";
	}
	else {
		error_string = "Not OK";
	}

	if (check_col(pinfo->fd, COL_INFO)) {
		col_add_fstr(pinfo->fd, COL_INFO, "R %s", error_string);
	}

	if (ncp_tree) {

		/* Put the func (and maybe subfunc) from the request packet
		 * in the proto tree, but hidden. That way filters on ncp.func
		 * or ncp.subfunc will find both the requests and the replies.
		 */
		if (ncp_rec) {
			proto_tree_add_uint(ncp_tree, hf_ncp_func, tvb,
					6, 0, ncp_rec->func);
			if (ncp_requires_subfunc(ncp_rec->func)) {
				proto_tree_add_uint(ncp_tree, hf_ncp_subfunc,
						tvb, 6, 0, ncp_rec->subfunc);
			}
		}

		proto_tree_add_uint_format(ncp_tree, hf_ncp_completion_code, tvb, 6, 1,
			completion_code, "Completion Code: 0x%02x (%s)",
			completion_code, error_string);

		proto_tree_add_item(ncp_tree, hf_ncp_connection_status, tvb, 7, 1, FALSE);

		length = tvb_length(tvb);
		if (!ncp_rec && length > 8) {
			proto_tree_add_text(ncp_tree, tvb, 8, length - 8,
					"No request record found. Parsing is impossible.");
		}
		else if (ncp_rec && ncp_rec->reply_ptvc) {
			/* If a non-zero completion code was found, it is
			 * legal to not have any fields, even if the packet
			 * type is defined as having fields. */
			if (completion_code != 0 && tvb_length(tvb) == 8) {
				return;
			}

			ptvc = ptvcursor_new(ncp_tree, tvb, 8);
			process_ptvc_record(ptvc, ncp_rec->reply_ptvc);
			ptvcursor_free(ptvc);
		}
	}
}
