/*
   Unix SMB/CIFS implementation.

   Associate async requests with ID.

   Copyright (C) Pavel Březina 2021

     ** NOTE! The following LGPL license applies to the tevent
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "tevent.h"
#include "tevent_internal.h"
#include "tevent_util.h"

uint32_t tevent_chain_id;

uint32_t tevent_set_chain_id(uint32_t id)
{
	uint32_t old = tevent_chain_id;
	tevent_chain_id = id;
	return old;
}

inline uint32_t tevent_get_chain_id()
{
	return tevent_chain_id;
}
