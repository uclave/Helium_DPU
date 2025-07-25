/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * interface.c: VNET interfaces/sub-interfaces
 *
 * Copyright (c) 2008 Eliot Dresselhaus
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <vnet/adj/adj.h>
#include <vnet/adj/adj_mcast.h>
#include <vnet/ip/ip.h>
#include <vnet/interface/rx_queue_funcs.h>
#include <vnet/interface/tx_queue_funcs.h>

/* *INDENT-OFF* */
VLIB_REGISTER_LOG_CLASS (if_default_log, static) = {
  .class_name = "interface",
};
/* *INDENT-ON* */

#define log_debug(fmt,...) vlib_log_debug(if_default_log.class, fmt, __VA_ARGS__)
#define log_err(fmt,...) vlib_log_err(if_default_log.class, fmt, __VA_ARGS__)

typedef enum vnet_interface_helper_flags_t_
{
  VNET_INTERFACE_SET_FLAGS_HELPER_IS_CREATE = (1 << 0),
  VNET_INTERFACE_SET_FLAGS_HELPER_WANT_REDISTRIBUTE = (1 << 1),
} vnet_interface_helper_flags_t;

static clib_error_t *vnet_hw_interface_set_flags_helper (vnet_main_t * vnm,
							 u32 hw_if_index,
							 vnet_hw_interface_flags_t
							 flags,
							 vnet_interface_helper_flags_t
							 helper_flags);

static clib_error_t *vnet_sw_interface_set_flags_helper (vnet_main_t * vnm,
							 u32 sw_if_index,
							 vnet_sw_interface_flags_t
							 flags,
							 vnet_interface_helper_flags_t
							 helper_flags);

static clib_error_t *vnet_hw_interface_set_class_helper (vnet_main_t * vnm,
							 u32 hw_if_index,
							 u32 hw_class_index,
							 u32 redistribute);

typedef struct
{
  /* Either sw or hw interface index. */
  u32 sw_hw_if_index;

  /* Flags. */
  u32 flags;
} vnet_sw_hw_interface_state_t;

static void
serialize_vec_vnet_sw_hw_interface_state (serialize_main_t * m, va_list * va)
{
  vnet_sw_hw_interface_state_t *s =
    va_arg (*va, vnet_sw_hw_interface_state_t *);
  u32 n = va_arg (*va, u32);
  u32 i;
  for (i = 0; i < n; i++)
    {
      serialize_integer (m, s[i].sw_hw_if_index,
			 sizeof (s[i].sw_hw_if_index));
      serialize_integer (m, s[i].flags, sizeof (s[i].flags));
    }
}

static void
unserialize_vec_vnet_sw_hw_interface_state (serialize_main_t * m,
					    va_list * va)
{
  vnet_sw_hw_interface_state_t *s =
    va_arg (*va, vnet_sw_hw_interface_state_t *);
  u32 n = va_arg (*va, u32);
  u32 i;
  for (i = 0; i < n; i++)
    {
      unserialize_integer (m, &s[i].sw_hw_if_index,
			   sizeof (s[i].sw_hw_if_index));
      unserialize_integer (m, &s[i].flags, sizeof (s[i].flags));
    }
}

static vnet_sw_interface_flags_t
vnet_hw_interface_flags_to_sw (vnet_hw_interface_flags_t hwf)
{
  vnet_sw_interface_flags_t swf = VNET_SW_INTERFACE_FLAG_NONE;

  if (hwf & VNET_HW_INTERFACE_FLAG_LINK_UP)
    swf |= VNET_SW_INTERFACE_FLAG_ADMIN_UP;

  return (swf);
}

void
serialize_vnet_interface_state (serialize_main_t * m, va_list * va)
{
  vnet_main_t *vnm = va_arg (*va, vnet_main_t *);
  vnet_sw_hw_interface_state_t *sts = 0, *st;
  vnet_sw_interface_t *sif;
  vnet_hw_interface_t *hif;
  vnet_interface_main_t *im = &vnm->interface_main;

  /* Serialize hardware interface classes since they may have changed.
     Must do this before sending up/down flags. */
  /* *INDENT-OFF* */
  pool_foreach (hif, im->hw_interfaces)  {
    vnet_hw_interface_class_t * hw_class = vnet_get_hw_interface_class (vnm, hif->hw_class_index);
    serialize_cstring (m, hw_class->name);
  }
  /* *INDENT-ON* */

  /* Send sw/hw interface state when non-zero. */
  /* *INDENT-OFF* */
  pool_foreach (sif, im->sw_interfaces)  {
    if (sif->flags != 0)
      {
	vec_add2 (sts, st, 1);
	st->sw_hw_if_index = sif->sw_if_index;
	st->flags = sif->flags;
      }
  }
  /* *INDENT-ON* */

  vec_serialize (m, sts, serialize_vec_vnet_sw_hw_interface_state);

  if (sts)
    vec_set_len (sts, 0);

  /* *INDENT-OFF* */
  pool_foreach (hif, im->hw_interfaces)  {
    if (hif->flags != 0)
      {
	vec_add2 (sts, st, 1);
	st->sw_hw_if_index = hif->hw_if_index;
	st->flags = vnet_hw_interface_flags_to_sw(hif->flags);
      }
  }
  /* *INDENT-ON* */

  vec_serialize (m, sts, serialize_vec_vnet_sw_hw_interface_state);

  vec_free (sts);
}

static vnet_hw_interface_flags_t
vnet_sw_interface_flags_to_hw (vnet_sw_interface_flags_t swf)
{
  vnet_hw_interface_flags_t hwf = VNET_HW_INTERFACE_FLAG_NONE;

  if (swf & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
    hwf |= VNET_HW_INTERFACE_FLAG_LINK_UP;

  return (hwf);
}

void
unserialize_vnet_interface_state (serialize_main_t * m, va_list * va)
{
  vnet_main_t *vnm = va_arg (*va, vnet_main_t *);
  vnet_sw_hw_interface_state_t *sts = 0, *st;

  /* First set interface hardware class. */
  {
    vnet_interface_main_t *im = &vnm->interface_main;
    vnet_hw_interface_t *hif;
    char *class_name;
    uword *p;
    clib_error_t *error;

    /* *INDENT-OFF* */
    pool_foreach (hif, im->hw_interfaces)  {
      unserialize_cstring (m, &class_name);
      p = hash_get_mem (im->hw_interface_class_by_name, class_name);
      if (p)
        {
          error = vnet_hw_interface_set_class_helper
            (vnm, hif->hw_if_index, p[0], /* redistribute */ 0);
        }
      else
        error = clib_error_return (0, "hw class %s AWOL?", class_name);

      if (error)
	clib_error_report (error);
      vec_free (class_name);
    }
    /* *INDENT-ON* */
  }

  vec_unserialize (m, &sts, unserialize_vec_vnet_sw_hw_interface_state);
  vec_foreach (st, sts)
    vnet_sw_interface_set_flags_helper (vnm, st->sw_hw_if_index, st->flags,
					/* no distribute */ 0);
  vec_free (sts);

  vec_unserialize (m, &sts, unserialize_vec_vnet_sw_hw_interface_state);
  vec_foreach (st, sts)
  {
    vnet_hw_interface_set_flags_helper
      (vnm, st->sw_hw_if_index, vnet_sw_interface_flags_to_hw (st->flags),
       /* no distribute */ 0);
  }
  vec_free (sts);
}

static clib_error_t *
call_elf_section_interface_callbacks (vnet_main_t * vnm, u32 if_index,
				      u32 flags,
				      _vnet_interface_function_list_elt_t **
				      elts)
{
  _vnet_interface_function_list_elt_t *elt;
  vnet_interface_function_priority_t prio;
  clib_error_t *error = 0;

  for (prio = VNET_ITF_FUNC_PRIORITY_LOW;
       prio <= VNET_ITF_FUNC_PRIORITY_HIGH; prio++)
    {
      elt = elts[prio];

      while (elt)
	{
	  error = elt->fp (vnm, if_index, flags);
	  if (error)
	    return error;
	  elt = elt->next_interface_function;
	}
    }
  return error;
}

static clib_error_t *
call_hw_interface_add_del_callbacks (vnet_main_t * vnm, u32 hw_if_index,
				     u32 is_create)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  vnet_hw_interface_class_t *hw_class =
    vnet_get_hw_interface_class (vnm, hi->hw_class_index);
  vnet_device_class_t *dev_class =
    vnet_get_device_class (vnm, hi->dev_class_index);
  clib_error_t *error = 0;

  if (hw_class->interface_add_del_function
      && (error =
	  hw_class->interface_add_del_function (vnm, hw_if_index, is_create)))
    return error;

  if (dev_class->interface_add_del_function
      && (error =
	  dev_class->interface_add_del_function (vnm, hw_if_index,
						 is_create)))
    return error;

  error = call_elf_section_interface_callbacks
    (vnm, hw_if_index, is_create, vnm->hw_interface_add_del_functions);

  return error;
}

static clib_error_t *
call_sw_interface_add_del_callbacks (vnet_main_t * vnm, u32 sw_if_index,
				     u32 is_create)
{
  return call_elf_section_interface_callbacks
    (vnm, sw_if_index, is_create, vnm->sw_interface_add_del_functions);
}

clib_error_t *
vnet_hw_interface_set_tc_flags (vnet_main_t * vnm, u32 hw_if_index,
				    vnet_hw_interface_flags_t flags)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  u32 mask;
  clib_error_t *error = 0;

  mask = VNET_HW_INTERFACE_FLAG_USE_TC;
  flags &= mask;
  if ((hi->flags & mask) == flags)
    goto done;
  hi->flags &= ~mask;
  hi->flags |= flags;

  done:
  if (error)
    log_err ("hw_set_flags_helper: %U", format_clib_error, error);
  return error;
}

static clib_error_t *
vnet_hw_interface_set_flags_helper (vnet_main_t * vnm, u32 hw_if_index,
				    vnet_hw_interface_flags_t flags,
				    vnet_interface_helper_flags_t
				    helper_flags)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  vnet_hw_interface_class_t *hw_class =
    vnet_get_hw_interface_class (vnm, hi->hw_class_index);
  u32 mask;
  clib_error_t *error = 0;
  u32 is_create =
    (helper_flags & VNET_INTERFACE_SET_FLAGS_HELPER_IS_CREATE) != 0;

  mask =
    (VNET_HW_INTERFACE_FLAG_LINK_UP | VNET_HW_INTERFACE_FLAG_DUPLEX_MASK);
  flags &= mask;

  /* Call hardware interface add/del callbacks. */
  if (is_create)
    call_hw_interface_add_del_callbacks (vnm, hw_if_index, is_create);

  /* Already in the desired state? */
  if (!is_create && (hi->flags & mask) == flags)
    goto done;

  if ((hi->flags & VNET_HW_INTERFACE_FLAG_LINK_UP) !=
      (flags & VNET_HW_INTERFACE_FLAG_LINK_UP))
    {
      /* Do hardware class (e.g. ethernet). */
      if (hw_class->link_up_down_function
	  && (error = hw_class->link_up_down_function (vnm, hw_if_index,
						       flags)))
	goto done;

      error = call_elf_section_interface_callbacks
	(vnm, hw_if_index, flags, vnm->hw_interface_link_up_down_functions);

      if (error)
	goto done;
    }

  hi->flags &= ~mask;
  hi->flags |= flags;

done:
  if (error)
    log_err ("hw_set_flags_helper: %U", format_clib_error, error);
  return error;
}

static clib_error_t *
vnet_sw_interface_set_flags_helper (vnet_main_t * vnm, u32 sw_if_index,
				    vnet_sw_interface_flags_t flags,
				    vnet_interface_helper_flags_t
				    helper_flags)
{
  vnet_sw_interface_t *si = vnet_get_sw_interface (vnm, sw_if_index);
  u32 mask;
  clib_error_t *error = 0;
  u32 is_create =
    (helper_flags & VNET_INTERFACE_SET_FLAGS_HELPER_IS_CREATE) != 0;
  u32 old_flags;

  mask = VNET_SW_INTERFACE_FLAG_ADMIN_UP | VNET_SW_INTERFACE_FLAG_PUNT;
  flags &= mask;

  if (is_create)
    {
      error =
	call_sw_interface_add_del_callbacks (vnm, sw_if_index, is_create);
      if (error)
	goto done;

      if (flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
	{
	  /* Notify everyone when the interface is created as admin up */
	  error = call_elf_section_interface_callbacks (vnm, sw_if_index,
							flags,
							vnm->
							sw_interface_admin_up_down_functions);
	  if (error)
	    goto done;
	}
    }
  else
    {
      vnet_sw_interface_t *si_sup = si;

      /* Check that super interface is in correct state. */
      if (si->type == VNET_SW_INTERFACE_TYPE_SUB)
	{
	  si_sup = vnet_get_sw_interface (vnm, si->sup_sw_if_index);

	  /* Check to see if we're bringing down the soft interface and if it's parent is up */
	  if ((flags != (si_sup->flags & mask)) &&
	      (!((flags == 0)
		 && ((si_sup->flags & mask) ==
		     VNET_SW_INTERFACE_FLAG_ADMIN_UP))))
	    {
	      error = clib_error_return (0, "super-interface %U must be %U",
					 format_vnet_sw_interface_name, vnm,
					 si_sup,
					 format_vnet_sw_interface_flags,
					 flags);
	      goto done;
	    }
	}

      /* Already in the desired state? */
      if ((si->flags & mask) == flags)
	goto done;

      /* Sub-interfaces of hardware interfaces that do no redistribute,
         do not redistribute themselves. */
      if (si_sup->type == VNET_SW_INTERFACE_TYPE_HARDWARE)
	{
	  vnet_hw_interface_t *hi =
	    vnet_get_hw_interface (vnm, si_sup->hw_if_index);
	  vnet_device_class_t *dev_class =
	    vnet_get_device_class (vnm, hi->dev_class_index);
	  if (!dev_class->redistribute)
	    helper_flags &=
	      ~VNET_INTERFACE_SET_FLAGS_HELPER_WANT_REDISTRIBUTE;
	}

      /* set the flags now before invoking the registered clients
       * so that the state they query is consistent with the state here notified */
      old_flags = si->flags;
      si->flags &= ~mask;
      si->flags |= flags;
      if ((flags | old_flags) & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
	error = call_elf_section_interface_callbacks
	  (vnm, sw_if_index, flags,
	   vnm->sw_interface_admin_up_down_functions);

      if (error)
	{
	  /* restore flags on error */
	  si->flags = old_flags;
	  goto done;
	}

      if (si->type == VNET_SW_INTERFACE_TYPE_HARDWARE)
	{
	  vnet_hw_interface_t *hi =
	    vnet_get_hw_interface (vnm, si->hw_if_index);
	  vnet_hw_interface_class_t *hw_class =
	    vnet_get_hw_interface_class (vnm, hi->hw_class_index);
	  vnet_device_class_t *dev_class =
	    vnet_get_device_class (vnm, hi->dev_class_index);

	  if ((flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP) &&
	      (si->flags & VNET_SW_INTERFACE_FLAG_ERROR))
	    {
	      error = clib_error_return (0, "Interface in the error state");
	      goto done;
	    }

	  /* update si admin up flag in advance if we are going admin down */
	  if (!(flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP))
	    si->flags &= ~VNET_SW_INTERFACE_FLAG_ADMIN_UP;

	  if (dev_class->admin_up_down_function
	      && (error = dev_class->admin_up_down_function (vnm,
							     si->hw_if_index,
							     flags)))
	    {
	      /* restore si admin up flag to it's original state on errors */
	      si->flags = old_flags;
	      goto done;
	    }

	  if (hw_class->admin_up_down_function
	      && (error = hw_class->admin_up_down_function (vnm,
							    si->hw_if_index,
							    flags)))
	    {
	      /* restore si admin up flag to it's original state on errors */
	      si->flags = old_flags;
	      goto done;
	    }

	  /* Admin down implies link down. */
	  if (!(flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
	      && (hi->flags & VNET_HW_INTERFACE_FLAG_LINK_UP))
	    vnet_hw_interface_set_flags_helper (vnm, si->hw_if_index,
						hi->flags &
						~VNET_HW_INTERFACE_FLAG_LINK_UP,
						helper_flags);
	  vnet_hw_if_update_runtime_data (vnm, si->hw_if_index);
	}
    }

  si->flags &= ~mask;
  si->flags |= flags;

done:
  if (error)
    log_err ("sw_set_flags_helper: %U", format_clib_error, error);
  return error;
}

clib_error_t *
vnet_hw_interface_set_flags (vnet_main_t * vnm, u32 hw_if_index,
			     vnet_hw_interface_flags_t flags)
{
  log_debug ("hw_set_flags: hw_if_index %u flags 0x%x", hw_if_index, flags);
  return vnet_hw_interface_set_flags_helper
    (vnm, hw_if_index, flags,
     VNET_INTERFACE_SET_FLAGS_HELPER_WANT_REDISTRIBUTE);
}

clib_error_t *
vnet_sw_interface_set_flags (vnet_main_t * vnm, u32 sw_if_index,
			     vnet_sw_interface_flags_t flags)
{
  log_debug ("sw_set_flags: sw_if_index %u flags 0x%x", sw_if_index, flags);
  return vnet_sw_interface_set_flags_helper
    (vnm, sw_if_index, flags,
     VNET_INTERFACE_SET_FLAGS_HELPER_WANT_REDISTRIBUTE);
}

void
vnet_sw_interface_admin_up (vnet_main_t * vnm, u32 sw_if_index)
{
  u32 flags = vnet_sw_interface_get_flags (vnm, sw_if_index);
  log_debug ("sw_admin_up: sw_if_index %u", sw_if_index);

  if (!(flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP))
    {
      flags |= VNET_SW_INTERFACE_FLAG_ADMIN_UP;
      vnet_sw_interface_set_flags (vnm, sw_if_index, flags);
    }
}

void
vnet_sw_interface_admin_down (vnet_main_t * vnm, u32 sw_if_index)
{
  u32 flags = vnet_sw_interface_get_flags (vnm, sw_if_index);
  log_debug ("sw_admin_down: sw_if_index %u", sw_if_index);

  if (flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
    {
      flags &= ~(VNET_SW_INTERFACE_FLAG_ADMIN_UP);
      vnet_sw_interface_set_flags (vnm, sw_if_index, flags);
    }
}

static void
vnet_if_update_lookup_tables (vnet_main_t *vnm, u32 sw_if_index)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_hw_interface_t *hi = vnet_get_sup_hw_interface (vnm, sw_if_index);

  vec_validate_init_empty (im->hw_if_index_by_sw_if_index, sw_if_index, ~0);
  vec_validate_init_empty (im->if_out_arc_end_next_index_by_sw_if_index,
			   sw_if_index, ~0);

  im->hw_if_index_by_sw_if_index[sw_if_index] = hi->hw_if_index;
  im->if_out_arc_end_next_index_by_sw_if_index[sw_if_index] =
    hi->if_out_arc_end_node_next_index;
}

static u32
vnet_create_sw_interface_no_callbacks (vnet_main_t * vnm,
				       vnet_sw_interface_t * template)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_sw_interface_t *sw;
  u32 sw_if_index;

  pool_get (im->sw_interfaces, sw);
  sw_if_index = sw - im->sw_interfaces;

  sw[0] = template[0];

  sw->flags = 0;
  sw->sw_if_index = sw_if_index;
  if (sw->type == VNET_SW_INTERFACE_TYPE_HARDWARE)
    sw->sup_sw_if_index = sw->sw_if_index;

  /* Allocate counters for this interface. */
  {
    u32 i;

    vnet_interface_counter_lock (im);

    for (i = 0; i < vec_len (im->sw_if_counters); i++)
      {
	vlib_validate_simple_counter (&im->sw_if_counters[i], sw_if_index);
	vlib_zero_simple_counter (&im->sw_if_counters[i], sw_if_index);
      }

    for (i = 0; i < vec_len (im->combined_sw_if_counters); i++)
      {
	vlib_validate_combined_counter (&im->combined_sw_if_counters[i],
					sw_if_index);
	vlib_zero_combined_counter (&im->combined_sw_if_counters[i],
				    sw_if_index);
      }

    vnet_interface_counter_unlock (im);
  }

  vnet_if_update_lookup_tables (vnm, sw_if_index);
  return sw_if_index;
}

clib_error_t *
vnet_create_sw_interface (vnet_main_t * vnm, vnet_sw_interface_t * template,
			  u32 * sw_if_index)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  clib_error_t *error;
  vnet_hw_interface_t *hi;
  vnet_device_class_t *dev_class;

  if (template->sub.eth.flags.two_tags == 1
      && template->sub.eth.flags.exact_match == 1
      && (template->sub.eth.flags.inner_vlan_id_any == 1
	  || template->sub.eth.flags.outer_vlan_id_any == 1))
    {
      char *str = "inner-dot1q any exact-match is unsupported";
      error = clib_error_return (0, str);
      log_err ("create_sw_interface: %s", str);
      return error;
    }

  hi = vnet_get_sup_hw_interface (vnm, template->sup_sw_if_index);
  dev_class = vnet_get_device_class (vnm, hi->dev_class_index);

  if (template->type == VNET_SW_INTERFACE_TYPE_SUB &&
      dev_class->subif_add_del_function)
    {
      error = dev_class->subif_add_del_function (vnm, hi->hw_if_index,
						 (struct vnet_sw_interface_t
						  *) template, 1);
      if (error)
	return error;
    }

  *sw_if_index = vnet_create_sw_interface_no_callbacks (vnm, template);
  error = vnet_sw_interface_set_flags_helper
    (vnm, *sw_if_index, template->flags,
     VNET_INTERFACE_SET_FLAGS_HELPER_IS_CREATE);

  if (error)
    {
      /* undo the work done by vnet_create_sw_interface_no_callbacks() */
      log_err ("create_sw_interface: set flags failed\n  %U",
	       format_clib_error, error);
      call_sw_interface_add_del_callbacks (vnm, *sw_if_index, 0);
      vnet_sw_interface_t *sw =
	pool_elt_at_index (im->sw_interfaces, *sw_if_index);
      pool_put (im->sw_interfaces, sw);
    }
  else
    {
      vnet_sw_interface_t *sw =
	pool_elt_at_index (im->sw_interfaces, *sw_if_index);
      log_debug ("create_sw_interface: interface %U (sw_if_index %u) created",
		 format_vnet_sw_interface_name, vnm, sw, *sw_if_index);
    }

  return error;
}

void
vnet_delete_sw_interface (vnet_main_t * vnm, u32 sw_if_index)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_sw_interface_t *sw =
    pool_elt_at_index (im->sw_interfaces, sw_if_index);

  log_debug ("delete_sw_interface: sw_if_index %u, name '%U'",
	     sw_if_index, format_vnet_sw_if_index_name, vnm, sw_if_index);

  /* Check if the interface has config and is removed from L2 BD or XConnect */
  vnet_clear_sw_interface_tag (vnm, sw_if_index);

  /* Bring down interface in case it is up. */
  if (sw->flags != 0)
    vnet_sw_interface_set_flags (vnm, sw_if_index, /* flags */ 0);

  call_sw_interface_add_del_callbacks (vnm, sw_if_index, /* is_create */ 0);

  pool_put (im->sw_interfaces, sw);
}

static clib_error_t *
call_sw_interface_mtu_change_callbacks (vnet_main_t * vnm, u32 sw_if_index)
{
  return call_elf_section_interface_callbacks
    (vnm, sw_if_index, 0, vnm->sw_interface_mtu_change_functions);
}

void
vnet_sw_interface_set_mtu (vnet_main_t * vnm, u32 sw_if_index, u32 mtu)
{
  vnet_sw_interface_t *si = vnet_get_sw_interface (vnm, sw_if_index);

  if (si->mtu[VNET_MTU_L3] != mtu)
    {
      si->mtu[VNET_MTU_L3] = mtu;
      log_debug ("set_mtu: interface %U, new mtu %u",
		 format_vnet_sw_if_index_name, vnm, sw_if_index, mtu);

      call_sw_interface_mtu_change_callbacks (vnm, sw_if_index);
    }
}

void
vnet_sw_interface_set_protocol_mtu (vnet_main_t * vnm, u32 sw_if_index,
				    u32 mtu[])
{
  vnet_sw_interface_t *si = vnet_get_sw_interface (vnm, sw_if_index);
  bool changed = false;
  int i;

  for (i = 0; i < VNET_N_MTU; i++)
    {
      if (si->mtu[i] != mtu[i])
	{
	  si->mtu[i] = mtu[i];
	  changed = true;
	}
    }
  /* Notify interested parties */
  if (changed)
    {
      log_debug ("set_protocol_mtu: interface %U l3 %u ip4 %u ip6 %u mpls %u",
		 format_vnet_sw_if_index_name, vnm, sw_if_index,
		 mtu[VNET_MTU_L3], mtu[VNET_MTU_IP4], mtu[VNET_MTU_IP6],
		 mtu[VNET_MTU_MPLS]);
      call_sw_interface_mtu_change_callbacks (vnm, sw_if_index);
    }
}

void
vnet_sw_interface_ip_directed_broadcast (vnet_main_t * vnm,
					 u32 sw_if_index, u8 enable)
{
  vnet_sw_interface_t *si;

  si = vnet_get_sw_interface (vnm, sw_if_index);

  if (enable)
    si->flags |= VNET_SW_INTERFACE_FLAG_DIRECTED_BCAST;
  else
    si->flags &= ~VNET_SW_INTERFACE_FLAG_DIRECTED_BCAST;

  ip4_directed_broadcast (sw_if_index, enable);
}

/*
 * Reflect a change in hardware MTU on protocol MTUs
 */
static walk_rc_t
sw_interface_walk_callback (vnet_main_t * vnm, u32 sw_if_index, void *ctx)
{
  u32 *link_mtu = ctx;
  vnet_sw_interface_set_mtu (vnm, sw_if_index, *link_mtu);
  return WALK_CONTINUE;
}

clib_error_t *
vnet_hw_interface_set_max_frame_size (vnet_main_t *vnm, u32 hw_if_index,
				      u32 fs)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  vnet_hw_interface_class_t *hw_if_class =
    vnet_get_hw_interface_class (vnm, hi->hw_class_index);
  clib_error_t *err = 0;
  log_debug ("set_max_frame_size: interface %v, max_frame_size %u -> %u",
	     hi->name, hi->max_frame_size, fs);

  if (hw_if_class->set_max_frame_size == 0)
    return vnet_error (VNET_ERR_UNSUPPORTED,
		       "hw class doesn't support changing Max Frame Size");

  if (hi->max_frame_size != fs)
    {
      u32 mtu;
      if (hw_if_class->set_max_frame_size)
	if ((err = hw_if_class->set_max_frame_size (vnm, hi, fs)))
	  return err;
      hi->max_frame_size = fs;
      mtu = fs - hi->frame_overhead;
      vnet_hw_interface_walk_sw (vnm, hw_if_index, sw_interface_walk_callback,
				 &mtu);
    }
  return 0;
}
clib_error_t *
vnet_hw_interface_set_mtu (vnet_main_t *vnm, u32 hw_if_index, u32 mtu)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  return vnet_hw_interface_set_max_frame_size (vnm, hw_if_index,
					       mtu + hi->frame_overhead);
}

static void
setup_tx_node (vlib_main_t * vm,
	       u32 node_index, vnet_device_class_t * dev_class)
{
  vlib_node_t *n = vlib_get_node (vm, node_index);

  n->format_trace = dev_class->format_tx_trace;

  vlib_register_errors (vm, node_index, dev_class->tx_function_n_errors,
			dev_class->tx_function_error_strings,
			dev_class->tx_function_error_counters);
}

static void
setup_output_node (vlib_main_t * vm,
		   u32 node_index, vnet_hw_interface_class_t * hw_class)
{
  vlib_node_t *n = vlib_get_node (vm, node_index);
  n->format_buffer = hw_class->format_header;
  n->unformat_buffer = hw_class->unformat_header;
}

void
vnet_reset_interface_l3_output_node (vlib_main_t *vm, u32 sw_if_index)
{
  vnet_set_interface_l3_output_node (vm, sw_if_index,
				     (u8 *) "interface-output");
}

void
vnet_set_interface_l3_output_node (vlib_main_t *vm, u32 sw_if_index,
				   u8 *output_node)
{
  vlib_node_t *l3_node;

  l3_node = vlib_get_node_by_name (vm, output_node);

  static char *arcs[] = {
    "ip4-output",
    "ip6-output",
    "mpls-output",
    "ethernet-output",
  };
  u8 a;

  for (a = 0; a < ARRAY_LEN (arcs); a++)
    {
      u8 arc = vnet_get_feature_arc_index (arcs[a]);
      vnet_feature_modify_end_node (arc, sw_if_index, l3_node->index);
    }
}

/* Register an interface instance. */
u32
vnet_register_interface (vnet_main_t * vnm,
			 u32 dev_class_index,
			 u32 dev_instance,
			 u32 hw_class_index, u32 hw_instance)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_hw_interface_t *hw;
  vnet_device_class_t *dev_class =
    vnet_get_device_class (vnm, dev_class_index);
  vnet_hw_interface_class_t *hw_class =
    vnet_get_hw_interface_class (vnm, hw_class_index);
  vlib_main_t *vm = vnm->vlib_main;
  vnet_feature_config_main_t *fcm;
  vnet_config_main_t *cm;
  u32 hw_index, i;
  vlib_node_t *if_out_node =
    vlib_get_node (vm, vnet_interface_output_node.index);

  pool_get (im->hw_interfaces, hw);
  clib_memset (hw, 0, sizeof (*hw));
  hw->trace_classify_table_index = ~0;

  hw_index = hw - im->hw_interfaces;
  hw->hw_if_index = hw_index;
  hw->default_rx_mode = VNET_HW_IF_RX_MODE_POLLING;

  if (hw_class->tx_hash_fn_type == VNET_HASH_FN_TYPE_ETHERNET ||
      hw_class->tx_hash_fn_type == VNET_HASH_FN_TYPE_IP)
    hw->hf = vnet_hash_default_function (hw_class->tx_hash_fn_type);

  if (dev_class->format_device_name)
    hw->name = format (0, "%U", dev_class->format_device_name, dev_instance);
  else if (hw_class->format_interface_name)
    hw->name = format (0, "%U", hw_class->format_interface_name,
		       dev_instance);
  else
    hw->name = format (0, "%s%x", hw_class->name, dev_instance);

  if (!im->hw_interface_by_name)
    im->hw_interface_by_name = hash_create_vec ( /* size */ 0,
						sizeof (hw->name[0]),
						sizeof (uword));

  hash_set_mem (im->hw_interface_by_name, hw->name, hw_index);

  /* Make hardware interface point to software interface. */
  {
    vnet_sw_interface_t sw = {
      .type = VNET_SW_INTERFACE_TYPE_HARDWARE,
      .flood_class = VNET_FLOOD_CLASS_NORMAL,
      .hw_if_index = hw_index
    };
    hw->sw_if_index = vnet_create_sw_interface_no_callbacks (vnm, &sw);
  }

  hw->dev_class_index = dev_class_index;
  hw->dev_instance = dev_instance;
  hw->hw_class_index = hw_class_index;
  hw->hw_instance = hw_instance;

  hw->max_rate_bits_per_sec = 0;
  vnet_sw_interface_set_mtu (vnm, hw->sw_if_index, 0);

  if (dev_class->tx_function == 0 && dev_class->tx_fn_registrations == 0)
    goto no_output_nodes;	/* No output/tx nodes to create */

  /* If we have previously deleted interface nodes, re-use them. */
  if (vec_len (im->deleted_hw_interface_nodes) > 0)
    {
      vnet_hw_interface_nodes_t *hn;
      vlib_node_t *node;
      vlib_node_runtime_t *nrt;

      hn = vec_end (im->deleted_hw_interface_nodes) - 1;

      hw->tx_node_index = hn->tx_node_index;
      hw->output_node_index = hn->output_node_index;

      vlib_node_rename (vm, hw->tx_node_index, "%v-tx", hw->name);
      vlib_node_rename (vm, hw->output_node_index, "%v-output", hw->name);

      foreach_vlib_main ()
	{
	  vnet_interface_output_runtime_t *rt;

	  rt =
	    vlib_node_get_runtime_data (this_vlib_main, hw->output_node_index);
	  ASSERT (rt->is_deleted == 1);
	  rt->is_deleted = 0;
	  rt->hw_if_index = hw_index;
	  rt->sw_if_index = hw->sw_if_index;
	  rt->dev_instance = hw->dev_instance;

	  rt = vlib_node_get_runtime_data (this_vlib_main, hw->tx_node_index);
	  rt->hw_if_index = hw_index;
	  rt->sw_if_index = hw->sw_if_index;
	  rt->dev_instance = hw->dev_instance;
	}

      /* The new class may differ from the old one.
       * Functions have to be updated. */
      node = vlib_get_node (vm, hw->output_node_index);
      node->format_trace = format_vnet_interface_output_trace;
      node->node_fn_registrations = if_out_node->node_fn_registrations;
      node->function = if_out_node->function;

      foreach_vlib_main ()
	{
	  nrt = vlib_node_get_runtime (this_vlib_main, hw->output_node_index);
	  nrt->function = node->function;
	  vlib_node_runtime_perf_counter (this_vlib_main, nrt, 0, 0, 0,
					  VLIB_NODE_RUNTIME_PERF_RESET);
	}

      node = vlib_get_node (vm, hw->tx_node_index);
      if (dev_class->tx_fn_registrations)
	{
	  node->node_fn_registrations = dev_class->tx_fn_registrations;
	  node->function = vlib_node_get_preferred_node_fn_variant (
	    vm, dev_class->tx_fn_registrations);
	}
      else
	node->function = dev_class->tx_function;
      node->format_trace = dev_class->format_tx_trace;

      foreach_vlib_main ()
	{
	  nrt = vlib_node_get_runtime (this_vlib_main, hw->tx_node_index);
	  nrt->function = node->function;
	  vlib_node_runtime_perf_counter (this_vlib_main, nrt, 0, 0, 0,
					  VLIB_NODE_RUNTIME_PERF_RESET);
	}

      vec_dec_len (im->deleted_hw_interface_nodes, 1);
    }
  else
    {
      vlib_node_registration_t r;
      vnet_interface_output_runtime_t rt = {
	.hw_if_index = hw_index,
	.sw_if_index = hw->sw_if_index,
	.dev_instance = hw->dev_instance,
	.is_deleted = 0,
      };

      clib_memset (&r, 0, sizeof (r));
      r.type = VLIB_NODE_TYPE_INTERNAL;
      r.runtime_data = &rt;
      r.runtime_data_bytes = sizeof (rt);
      r.scalar_size = sizeof (vnet_hw_if_tx_frame_t);
      r.vector_size = sizeof (u32);

      r.flags = VLIB_NODE_FLAG_IS_OUTPUT;
      if (dev_class->tx_fn_registrations)
	{
	  r.function = 0;
	  r.node_fn_registrations = dev_class->tx_fn_registrations;
	}
      else
	r.function = dev_class->tx_function;

      hw->tx_node_index = vlib_register_node (vm, &r, "%v-tx", hw->name);

      vlib_node_add_named_next_with_slot (vm, hw->tx_node_index,
					  "error-drop",
					  VNET_INTERFACE_TX_NEXT_DROP);

      r.flags = 0;
      r.format_trace = format_vnet_interface_output_trace;
      if (if_out_node->node_fn_registrations)
	{
	  r.function = 0;
	  r.node_fn_registrations = if_out_node->node_fn_registrations;
	}
      else
	r.function = if_out_node->function;

      {
	static char *e[] = {
	  "interface is down",
	  "interface is deleted",
	  "no tx queue available",
	};

	r.n_errors = ARRAY_LEN (e);
	r.error_strings = e;
      }
      hw->output_node_index =
	vlib_register_node (vm, &r, "%v-output", hw->name);

      vlib_node_add_named_next_with_slot (vm, hw->output_node_index,
					  "error-drop",
					  VNET_INTERFACE_OUTPUT_NEXT_DROP);
      vlib_node_add_next_with_slot (vm, hw->output_node_index,
				    hw->tx_node_index,
				    VNET_INTERFACE_OUTPUT_NEXT_TX);
      /* add interface to the list of "output-interface" feature arc start nodes
         and clone nexts from 1st interface if it exists */
      fcm = vnet_feature_get_config_main (im->output_feature_arc_index);
      cm = &fcm->config_main;
      i = vec_len (cm->start_node_indices);
      vec_validate (cm->start_node_indices, i);
      cm->start_node_indices[i] = hw->output_node_index;
      if (hw_index)
	{
	  /* copy nexts from 1st interface */
	  vnet_hw_interface_t *first_hw;
	  vlib_node_t *first_node;

	  first_hw = vnet_get_hw_interface (vnm, /* hw_if_index */ 0);
	  first_node = vlib_get_node (vm, first_hw->output_node_index);

	  /* 1st 2 nexts are already added above */
	  for (i = 2; i < vec_len (first_node->next_nodes); i++)
	    vlib_node_add_next_with_slot (vm, hw->output_node_index,
					  first_node->next_nodes[i], i);
	}
    }

  hw->if_out_arc_end_node_next_index = vlib_node_add_next (
    vm, vnet_interface_output_arc_end_node.index, hw->tx_node_index);
  vnet_if_update_lookup_tables (vnm, hw->sw_if_index);
  setup_output_node (vm, hw->output_node_index, hw_class);
  setup_tx_node (vm, hw->tx_node_index, dev_class);

no_output_nodes:
  /* Call all up/down callbacks with zero flags when interface is created. */
  vnet_sw_interface_set_flags_helper (vnm, hw->sw_if_index, /* flags */ 0,
				      VNET_INTERFACE_SET_FLAGS_HELPER_IS_CREATE);
  vnet_hw_interface_set_flags_helper (vnm, hw_index, /* flags */ 0,
				      VNET_INTERFACE_SET_FLAGS_HELPER_IS_CREATE);
  return hw_index;
}

void
vnet_delete_hw_interface (vnet_main_t * vnm, u32 hw_if_index)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_hw_interface_t *hw = vnet_get_hw_interface (vnm, hw_if_index);
  vlib_main_t *vm = vnm->vlib_main;
  vnet_device_class_t *dev_class = vnet_get_device_class (vnm,
							  hw->dev_class_index);
  /* If it is up, mark it down. */
  if (hw->flags != 0)
    vnet_hw_interface_set_flags (vnm, hw_if_index, /* flags */ 0);

  /* Call delete callbacks. */
  call_hw_interface_add_del_callbacks (vnm, hw_if_index, /* is_create */ 0);

  /* delete rx & tx queues */
  vnet_hw_if_unregister_all_rx_queues (vnm, hw_if_index);
  vnet_hw_if_unregister_all_tx_queues (vnm, hw_if_index);
  vnet_hw_if_update_runtime_data (vnm, hw_if_index);

  /* Delete any sub-interfaces. */
  {
    u32 id, sw_if_index;
    /* *INDENT-OFF* */
    hash_foreach (id, sw_if_index, hw->sub_interface_sw_if_index_by_id,
    ({
      vnet_sw_interface_t *si = vnet_get_sw_interface (vnm, sw_if_index);
      u64 sup_and_sub_key =
	((u64) (si->sup_sw_if_index) << 32) | (u64) si->sub.id;
      hash_unset_mem_free (&im->sw_if_index_by_sup_and_sub, &sup_and_sub_key);
      vnet_delete_sw_interface (vnm, sw_if_index);
    }));
    hash_free (hw->sub_interface_sw_if_index_by_id);
    /* *INDENT-ON* */
  }

  /* Delete software interface corresponding to hardware interface. */
  vnet_delete_sw_interface (vnm, hw->sw_if_index);

  if (dev_class->tx_function)
    {
      /* Put output/tx nodes into recycle pool */
      vnet_hw_interface_nodes_t *dn;

      foreach_vlib_main ()
	{
	  vnet_interface_output_runtime_t *rt =
	    vlib_node_get_runtime_data (this_vlib_main, hw->output_node_index);

	  /* Mark node runtime as deleted so output node (if called)
	   * will drop packets. */
	  rt->is_deleted = 1;
	}

      vlib_node_rename (vm, hw->output_node_index,
			"interface-%d-output-deleted", hw_if_index);
      vlib_node_rename (vm, hw->tx_node_index, "interface-%d-tx-deleted",
			hw_if_index);
      vlib_unregister_errors (vm, hw->output_node_index);
      vlib_unregister_errors (vm, hw->tx_node_index);
      vec_add2 (im->deleted_hw_interface_nodes, dn, 1);
      dn->tx_node_index = hw->tx_node_index;
      dn->output_node_index = hw->output_node_index;
    }
  hash_unset_mem (im->hw_interface_by_name, hw->name);
  vec_free (hw->name);
  vec_free (hw->hw_address);
  vec_free (hw->output_node_thread_runtimes);
  pool_put (im->hw_interfaces, hw);
}

void
vnet_hw_interface_walk_sw (vnet_main_t * vnm,
			   u32 hw_if_index,
			   vnet_hw_sw_interface_walk_t fn, void *ctx)
{
  vnet_hw_interface_t *hi;
  u32 id, sw_if_index;

  hi = vnet_get_hw_interface (vnm, hw_if_index);
  /* the super first, then the sub interfaces */
  if (WALK_STOP == fn (vnm, hi->sw_if_index, ctx))
    return;

  /* *INDENT-OFF* */
  hash_foreach (id, sw_if_index,
                hi->sub_interface_sw_if_index_by_id,
  ({
    if (WALK_STOP == fn (vnm, sw_if_index, ctx))
      break;
  }));
  /* *INDENT-ON* */
}

void
vnet_hw_interface_walk (vnet_main_t * vnm,
			vnet_hw_interface_walk_t fn, void *ctx)
{
  vnet_interface_main_t *im;
  vnet_hw_interface_t *hi;

  im = &vnm->interface_main;

  /* *INDENT-OFF* */
  pool_foreach (hi, im->hw_interfaces)
   {
    if (WALK_STOP == fn(vnm, hi->hw_if_index, ctx))
      break;
  }
  /* *INDENT-ON* */
}

void
vnet_sw_interface_walk (vnet_main_t * vnm,
			vnet_sw_interface_walk_t fn, void *ctx)
{
  vnet_interface_main_t *im;
  vnet_sw_interface_t *si;

  im = &vnm->interface_main;

  /* *INDENT-OFF* */
  pool_foreach (si, im->sw_interfaces)
  {
    if (WALK_STOP == fn (vnm, si, ctx))
      break;
  }
  /* *INDENT-ON* */
}

void
vnet_hw_interface_init_for_class (vnet_main_t * vnm, u32 hw_if_index,
				  u32 hw_class_index, u32 hw_instance)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  vnet_hw_interface_class_t *hc =
    vnet_get_hw_interface_class (vnm, hw_class_index);

  hi->hw_class_index = hw_class_index;
  hi->hw_instance = hw_instance;
  setup_output_node (vnm->vlib_main, hi->output_node_index, hc);
}

static clib_error_t *
vnet_hw_interface_set_class_helper (vnet_main_t * vnm, u32 hw_if_index,
				    u32 hw_class_index, u32 redistribute)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  vnet_sw_interface_t *si = vnet_get_sw_interface (vnm, hi->sw_if_index);
  vnet_hw_interface_class_t *old_class =
    vnet_get_hw_interface_class (vnm, hi->hw_class_index);
  vnet_hw_interface_class_t *new_class =
    vnet_get_hw_interface_class (vnm, hw_class_index);
  vnet_device_class_t *dev_class =
    vnet_get_device_class (vnm, hi->dev_class_index);
  clib_error_t *error = 0;

  /* New class equals old class?  Nothing to do. */
  if (hi->hw_class_index == hw_class_index)
    return 0;

  /* No need (and incorrect since admin up flag may be set) to do error checking when
     receiving unserialize message. */
  if (redistribute)
    {
      if (si->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP)
	return clib_error_return (0,
				  "%v must be admin down to change class from %s to %s",
				  hi->name, old_class->name, new_class->name);

      /* Make sure interface supports given class. */
      if ((new_class->is_valid_class_for_interface
	   && !new_class->is_valid_class_for_interface (vnm, hw_if_index,
							hw_class_index))
	  || (dev_class->is_valid_class_for_interface
	      && !dev_class->is_valid_class_for_interface (vnm, hw_if_index,
							   hw_class_index)))
	return clib_error_return (0,
				  "%v class cannot be changed from %s to %s",
				  hi->name, old_class->name, new_class->name);

    }

  if (old_class->hw_class_change)
    old_class->hw_class_change (vnm, hw_if_index, old_class->index,
				new_class->index);

  vnet_hw_interface_init_for_class (vnm, hw_if_index, new_class->index,
				    /* instance */ ~0);

  if (new_class->hw_class_change)
    new_class->hw_class_change (vnm, hw_if_index, old_class->index,
				new_class->index);

  if (dev_class->hw_class_change)
    dev_class->hw_class_change (vnm, hw_if_index, new_class->index);

  return error;
}

clib_error_t *
vnet_hw_interface_set_class (vnet_main_t * vnm, u32 hw_if_index,
			     u32 hw_class_index)
{
  return vnet_hw_interface_set_class_helper (vnm, hw_if_index, hw_class_index,
					     /* redistribute */ 1);
}

static int
vnet_hw_interface_rx_redirect_to_node_helper (vnet_main_t * vnm,
					      u32 hw_if_index,
					      u32 node_index,
					      u32 redistribute)
{
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);
  vnet_device_class_t *dev_class = vnet_get_device_class
    (vnm, hi->dev_class_index);

  if (dev_class->rx_redirect_to_node)
    {
      dev_class->rx_redirect_to_node (vnm, hw_if_index, node_index);
      return 0;
    }

  return VNET_API_ERROR_UNIMPLEMENTED;
}

int
vnet_hw_interface_rx_redirect_to_node (vnet_main_t * vnm, u32 hw_if_index,
				       u32 node_index)
{
  return vnet_hw_interface_rx_redirect_to_node_helper (vnm, hw_if_index,
						       node_index,
						       1 /* redistribute */ );
}

word
vnet_sw_interface_compare (vnet_main_t * vnm,
			   uword sw_if_index0, uword sw_if_index1)
{
  vnet_sw_interface_t *sup0 = vnet_get_sup_sw_interface (vnm, sw_if_index0);
  vnet_sw_interface_t *sup1 = vnet_get_sup_sw_interface (vnm, sw_if_index1);
  vnet_hw_interface_t *h0 = vnet_get_hw_interface (vnm, sup0->hw_if_index);
  vnet_hw_interface_t *h1 = vnet_get_hw_interface (vnm, sup1->hw_if_index);

  if (h0 != h1)
    return vec_cmp (h0->name, h1->name);
  return (word) h0->hw_instance - (word) h1->hw_instance;
}

word
vnet_hw_interface_compare (vnet_main_t * vnm,
			   uword hw_if_index0, uword hw_if_index1)
{
  vnet_hw_interface_t *h0 = vnet_get_hw_interface (vnm, hw_if_index0);
  vnet_hw_interface_t *h1 = vnet_get_hw_interface (vnm, hw_if_index1);

  if (h0 != h1)
    return vec_cmp (h0->name, h1->name);
  return (word) h0->hw_instance - (word) h1->hw_instance;
}

int
vnet_sw_interface_is_p2p (vnet_main_t * vnm, u32 sw_if_index)
{
  vnet_sw_interface_t *si = vnet_get_sw_interface_or_null (vnm, sw_if_index);
  if (si == NULL)
    return -1;

  if ((si->type == VNET_SW_INTERFACE_TYPE_P2P) ||
      (si->type == VNET_SW_INTERFACE_TYPE_PIPE))
    return 1;

  vnet_hw_interface_t *hw = vnet_get_sup_hw_interface (vnm, sw_if_index);
  vnet_hw_interface_class_t *hc =
    vnet_get_hw_interface_class (vnm, hw->hw_class_index);

  return (hc->flags & VNET_HW_INTERFACE_CLASS_FLAG_P2P);
}

int
vnet_sw_interface_is_nbma (vnet_main_t * vnm, u32 sw_if_index)
{
  vnet_hw_interface_t *hw = vnet_get_sup_hw_interface (vnm, sw_if_index);
  vnet_hw_interface_class_t *hc =
    vnet_get_hw_interface_class (vnm, hw->hw_class_index);

  return (hc->flags & VNET_HW_INTERFACE_CLASS_FLAG_NBMA);
}

clib_error_t *
vnet_sw_interface_supports_addressing (vnet_main_t *vnm, u32 sw_if_index)
{
  if (sw_if_index == 0)
    {
      return clib_error_create (
	"local0 interface doesn't support IP addressing");
    }

  if (vnet_sw_interface_is_sub (vnm, sw_if_index))
    {
      vnet_sw_interface_t *si;
      si = vnet_get_sw_interface_or_null (vnm, sw_if_index);
      if (si && si->type == VNET_SW_INTERFACE_TYPE_SUB &&
	  si->sub.eth.flags.exact_match == 0)
	{
	  return clib_error_create (
	    "sub-interface without exact-match doesn't support IP addressing");
	}
    }
  return NULL;
}

u32
vnet_register_device_class (vlib_main_t *vm, vnet_device_class_t *c)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  c->index = vec_len (im->device_classes);
  hash_set_mem (im->device_class_by_name, c->name, c->index);

  /* to avoid confusion, please remove ".tx_function" statement
    from VNET_DEVICE_CLASS() if using function candidates */
  ASSERT (c->tx_fn_registrations == 0 || c->tx_function == 0);

  if (c->tx_fn_registrations)
    c->tx_function =
      vlib_node_get_preferred_node_fn_variant (vm, c->tx_fn_registrations);

  vec_add1 (im->device_classes, c[0]);
  return c->index;
}

clib_error_t *
vnet_interface_init (vlib_main_t * vm)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  vlib_buffer_t *b = 0;
  vnet_buffer_opaque_t *o = 0;
  clib_error_t *error;

  /*
   * Keep people from shooting themselves in the foot.
   */
  if (sizeof (b->opaque) != sizeof (vnet_buffer_opaque_t))
    {
#define _(a) if (sizeof(o->a) > sizeof (o->unused))                     \
      clib_warning                                                      \
        ("FATAL: size of opaque union subtype %s is %d (max %d)",       \
         #a, sizeof(o->a), sizeof (o->unused));
      foreach_buffer_opaque_union_subtype;
#undef _

      return clib_error_return
	(0, "FATAL: size of vlib buffer opaque %d, size of vnet opaque %d",
	 sizeof (b->opaque), sizeof (vnet_buffer_opaque_t));
    }

  clib_spinlock_init (&im->sw_if_counter_lock);
  clib_spinlock_lock (&im->sw_if_counter_lock);	/* should be no need */

  vec_validate (im->sw_if_counters, VNET_N_SIMPLE_INTERFACE_COUNTER - 1);
#define _(E,n,p)							\
  im->sw_if_counters[VNET_INTERFACE_COUNTER_##E].name = #n;		\
  im->sw_if_counters[VNET_INTERFACE_COUNTER_##E].stat_segment_name = "/" #p "/" #n;
  foreach_simple_interface_counter_name
#undef _
    vec_validate (im->combined_sw_if_counters,
		  VNET_N_COMBINED_INTERFACE_COUNTER - 1);
#define _(E,n,p)							\
  im->combined_sw_if_counters[VNET_INTERFACE_COUNTER_##E].name = #n;	\
  im->combined_sw_if_counters[VNET_INTERFACE_COUNTER_##E].stat_segment_name = "/" #p "/" #n;
  foreach_combined_interface_counter_name
#undef _
    clib_spinlock_unlock (&im->sw_if_counter_lock);

  im->device_class_by_name = hash_create_string ( /* size */ 0,
						 sizeof (uword));

  for (vnet_device_class_t *c = vnm->device_class_registrations; c;
       c = c->next_class_registration)
    vnet_register_device_class (vm, c);

  im->hw_interface_class_by_name = hash_create_string ( /* size */ 0,
						       sizeof (uword));

  im->rxq_index_by_hw_if_index_and_queue_id =
    hash_create_mem (0, sizeof (u64), sizeof (u32));
  im->txq_index_by_hw_if_index_and_queue_id =
    hash_create_mem (0, sizeof (u64), sizeof (u32));
  im->sw_if_index_by_sup_and_sub = hash_create_mem (0, sizeof (u64),
						    sizeof (uword));
  {
    vnet_hw_interface_class_t *c;

    c = vnm->hw_interface_class_registrations;

    while (c)
      {
	c->index = vec_len (im->hw_interface_classes);
	hash_set_mem (im->hw_interface_class_by_name, c->name, c->index);

	if (NULL == c->build_rewrite)
	  c->build_rewrite = default_build_rewrite;
	if (NULL == c->update_adjacency)
	  c->update_adjacency = default_update_adjacency;

	vec_add1 (im->hw_interface_classes, c[0]);
	c = c->next_class_registration;
      }
  }

  /* init per-thread data */
  vec_validate_aligned (im->per_thread_data, vlib_num_workers (),
			CLIB_CACHE_LINE_BYTES);

  if ((error = vlib_call_init_function (vm, vnet_interface_cli_init)))
    return error;

  vnm->interface_tag_by_sw_if_index = hash_create (0, sizeof (uword));

  return 0;
}

VLIB_INIT_FUNCTION (vnet_interface_init);

/* Kludge to renumber interface names [only!] */
int
vnet_interface_name_renumber (u32 sw_if_index, u32 new_show_dev_instance)
{
  int rv;
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_hw_interface_t *hi = vnet_get_sup_hw_interface (vnm, sw_if_index);

  vnet_device_class_t *dev_class = vnet_get_device_class
    (vnm, hi->dev_class_index);

  if (dev_class->name_renumber == 0 || dev_class->format_device_name == 0)
    return VNET_API_ERROR_UNIMPLEMENTED;

  rv = dev_class->name_renumber (hi, new_show_dev_instance);

  if (rv)
    return rv;

  hash_unset_mem (im->hw_interface_by_name, hi->name);
  vec_free (hi->name);
  /* Use the mapping we set up to call it Ishmael */
  hi->name = format (0, "%U", dev_class->format_device_name,
		     hi->dev_instance);

  hash_set_mem (im->hw_interface_by_name, hi->name, hi->hw_if_index);
  return rv;
}

clib_error_t *
vnet_rename_interface (vnet_main_t * vnm, u32 hw_if_index, char *new_name)
{
  vnet_interface_main_t *im = &vnm->interface_main;
  vlib_main_t *vm = vnm->vlib_main;
  vnet_hw_interface_t *hw;
  u8 *old_name;
  clib_error_t *error = 0;

  hw = vnet_get_hw_interface (vnm, hw_if_index);
  if (!hw)
    {
      return clib_error_return (0,
				"unable to find hw interface for index %u",
				hw_if_index);
    }

  old_name = hw->name;

  /* set new hw->name */
  hw->name = format (0, "%s", new_name);

  /* remove the old name to hw_if_index mapping and install the new one */
  hash_unset_mem (im->hw_interface_by_name, old_name);
  hash_set_mem (im->hw_interface_by_name, hw->name, hw_if_index);

  /* rename tx/output nodes */
  vlib_node_rename (vm, hw->tx_node_index, "%v-tx", hw->name);
  vlib_node_rename (vm, hw->output_node_index, "%v-output", hw->name);

  /* free the old name vector */
  vec_free (old_name);

  return error;
}

clib_error_t *
vnet_hw_interface_add_del_mac_address (vnet_main_t * vnm,
				       u32 hw_if_index,
				       const u8 * mac_address, u8 is_add)
{
  clib_error_t *error = 0;
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);

  vnet_device_class_t *dev_class =
    vnet_get_device_class (vnm, hi->dev_class_index);

  if (!hi->hw_address)
    {
      error =
	clib_error_return
	(0, "Secondary MAC Addresses not supported for interface index %u",
	 hw_if_index);
      goto done;
    }

  if (dev_class->mac_addr_add_del_function)
    error = dev_class->mac_addr_add_del_function (hi, mac_address, is_add);

  if (!error)
    {
      vnet_hw_interface_class_t *hw_class;

      hw_class = vnet_get_hw_interface_class (vnm, hi->hw_class_index);

      if (NULL != hw_class->mac_addr_add_del_function)
	error = hw_class->mac_addr_add_del_function (hi, mac_address, is_add);
    }

  /* If no errors, add to the list of secondary MACs on the ethernet intf */
  if (!error)
    ethernet_interface_add_del_address (&ethernet_main, hw_if_index,
					mac_address, is_add);

done:
  if (error)
    log_err ("hw_add_del_mac_address: %U", format_clib_error, error);
  return error;
}

static clib_error_t *
vnet_hw_interface_change_mac_address_helper (vnet_main_t * vnm,
					     u32 hw_if_index,
					     const u8 * mac_address)
{
  clib_error_t *error = 0;
  vnet_hw_interface_t *hi = vnet_get_hw_interface (vnm, hw_if_index);

  if (hi->hw_address)
    {
      u8 *old_address = vec_dup (hi->hw_address);
      vnet_device_class_t *dev_class =
	vnet_get_device_class (vnm, hi->dev_class_index);
      if (dev_class->mac_addr_change_function)
	{
	  error =
	    dev_class->mac_addr_change_function (hi, old_address,
						 mac_address);
	}
      if (!error)
	{
	  vnet_hw_interface_class_t *hw_class;

	  hw_class = vnet_get_hw_interface_class (vnm, hi->hw_class_index);

	  if (NULL != hw_class->mac_addr_change_function)
	    hw_class->mac_addr_change_function (hi, old_address, mac_address);
	}
      else
	{
	  error =
	    clib_error_return (0,
			       "MAC Address Change is not supported on this interface");
	}
      vec_free (old_address);
    }
  else
    {
      error =
	clib_error_return (0,
			   "mac address change is not supported for interface index %u",
			   hw_if_index);
    }
  return error;
}

clib_error_t *
vnet_hw_interface_change_mac_address (vnet_main_t * vnm, u32 hw_if_index,
				      const u8 * mac_address)
{
  return vnet_hw_interface_change_mac_address_helper
    (vnm, hw_if_index, mac_address);
}

static int
vnet_sw_interface_check_table_same (u32 unnumbered_sw_if_index,
				    u32 ip_sw_if_index)
{
  if (ip4_main.fib_index_by_sw_if_index[unnumbered_sw_if_index] !=
      ip4_main.fib_index_by_sw_if_index[ip_sw_if_index])
    return VNET_API_ERROR_UNEXPECTED_INTF_STATE;

  if (ip4_main.mfib_index_by_sw_if_index[unnumbered_sw_if_index] !=
      ip4_main.mfib_index_by_sw_if_index[ip_sw_if_index])
    return VNET_API_ERROR_UNEXPECTED_INTF_STATE;

  if (ip6_main.fib_index_by_sw_if_index[unnumbered_sw_if_index] !=
      ip6_main.fib_index_by_sw_if_index[ip_sw_if_index])
    return VNET_API_ERROR_UNEXPECTED_INTF_STATE;

  if (ip6_main.mfib_index_by_sw_if_index[unnumbered_sw_if_index] !=
      ip6_main.mfib_index_by_sw_if_index[ip_sw_if_index])
    return VNET_API_ERROR_UNEXPECTED_INTF_STATE;

  return 0;
}

/* update the unnumbered state of an interface*/
int
vnet_sw_interface_update_unnumbered (u32 unnumbered_sw_if_index,
				     u32 ip_sw_if_index, u8 enable)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_sw_interface_t *si;
  u32 was_unnum;
  int rv = 0;

  si = vnet_get_sw_interface (vnm, unnumbered_sw_if_index);
  was_unnum = (si->flags & VNET_SW_INTERFACE_FLAG_UNNUMBERED);

  if (enable)
    {
      rv = vnet_sw_interface_check_table_same (unnumbered_sw_if_index,
					       ip_sw_if_index);
      if (rv != 0)
	return rv;
      si->flags |= VNET_SW_INTERFACE_FLAG_UNNUMBERED;
      si->unnumbered_sw_if_index = ip_sw_if_index;

      ip4_main.lookup_main.if_address_pool_index_by_sw_if_index
	[unnumbered_sw_if_index] =
	ip4_main.
	lookup_main.if_address_pool_index_by_sw_if_index[ip_sw_if_index];
      ip6_main.
	lookup_main.if_address_pool_index_by_sw_if_index
	[unnumbered_sw_if_index] =
	ip6_main.
	lookup_main.if_address_pool_index_by_sw_if_index[ip_sw_if_index];
    }
  else
    {
      /*
       * Unless the interface is actually unnumbered, don't
       * smash e.g. if_address_pool_index_by_sw_if_index
       */
      if (si->flags & VNET_SW_INTERFACE_FLAG_UNNUMBERED)
	{
	  si->flags &= ~(VNET_SW_INTERFACE_FLAG_UNNUMBERED);
	  si->unnumbered_sw_if_index = (u32) ~0;

	  ip4_main.lookup_main
	    .if_address_pool_index_by_sw_if_index[unnumbered_sw_if_index] = ~0;
	  ip6_main.lookup_main
	    .if_address_pool_index_by_sw_if_index[unnumbered_sw_if_index] = ~0;
	}
    }

  if (was_unnum != (si->flags & VNET_SW_INTERFACE_FLAG_UNNUMBERED))
    {
      ip4_sw_interface_enable_disable (unnumbered_sw_if_index, enable);
      ip6_sw_interface_enable_disable (unnumbered_sw_if_index, enable);
    }

  return 0;
}

vnet_l3_packet_type_t
vnet_link_to_l3_proto (vnet_link_t link)
{
  switch (link)
    {
    case VNET_LINK_IP4:
      return (VNET_L3_PACKET_TYPE_IP4);
    case VNET_LINK_IP6:
      return (VNET_L3_PACKET_TYPE_IP6);
    case VNET_LINK_MPLS:
      return (VNET_L3_PACKET_TYPE_MPLS);
    case VNET_LINK_ARP:
      return (VNET_L3_PACKET_TYPE_ARP);
    case VNET_LINK_ETHERNET:
    case VNET_LINK_NSH:
      ASSERT (0);
      break;
    }
  ASSERT (0);
  return (0);
}

vnet_mtu_t
vnet_link_to_mtu (vnet_link_t link)
{
  switch (link)
    {
    case VNET_LINK_IP4:
      return (VNET_MTU_IP4);
    case VNET_LINK_IP6:
      return (VNET_MTU_IP6);
    case VNET_LINK_MPLS:
      return (VNET_MTU_MPLS);
    default:
      return (VNET_MTU_L3);
    }
}

u8 *
default_build_rewrite (vnet_main_t * vnm,
		       u32 sw_if_index,
		       vnet_link_t link_type, const void *dst_address)
{
  return (NULL);
}

void
default_update_adjacency (vnet_main_t * vnm, u32 sw_if_index, u32 ai)
{
  ip_adjacency_t *adj;

  adj = adj_get (ai);

  switch (adj->lookup_next_index)
    {
    case IP_LOOKUP_NEXT_GLEAN:
      adj_glean_update_rewrite (ai);
      break;
    case IP_LOOKUP_NEXT_ARP:
    case IP_LOOKUP_NEXT_BCAST:
      /*
       * default rewrite in neighbour adj
       */
      adj_nbr_update_rewrite
	(ai,
	 ADJ_NBR_REWRITE_FLAG_COMPLETE,
	 vnet_build_rewrite_for_sw_interface (vnm,
					      sw_if_index,
					      adj_get_link_type (ai), NULL));
      break;
    case IP_LOOKUP_NEXT_MCAST:
      /*
       * mcast traffic also uses default rewrite string with no mcast
       * switch time updates.
       */
      adj_mcast_update_rewrite
	(ai,
	 vnet_build_rewrite_for_sw_interface (vnm,
					      sw_if_index,
					      adj_get_link_type (ai),
					      NULL), 0);
      break;
    case IP_LOOKUP_NEXT_DROP:
    case IP_LOOKUP_NEXT_PUNT:
    case IP_LOOKUP_NEXT_LOCAL:
    case IP_LOOKUP_NEXT_REWRITE:
    case IP_LOOKUP_NEXT_MCAST_MIDCHAIN:
    case IP_LOOKUP_NEXT_MIDCHAIN:
    case IP_LOOKUP_NEXT_ICMP_ERROR:
    case IP_LOOKUP_N_NEXT:
      ASSERT (0);
      break;
    }
}

clib_error_t *
vnet_hw_interface_set_rss_queues (vnet_main_t * vnm,
				  vnet_hw_interface_t * hi,
				  clib_bitmap_t * bitmap)
{
  clib_error_t *error = 0;
  vnet_device_class_t *dev_class =
    vnet_get_device_class (vnm, hi->dev_class_index);

  if (dev_class->set_rss_queues_function)
    {
      if (clib_bitmap_count_set_bits (bitmap) == 0)
	{
	  error = clib_error_return (0,
				     "must assign at least one valid rss queue");
	  goto done;
	}

      error = dev_class->set_rss_queues_function (vnm, hi, bitmap);
    }
  else
    {
      error = clib_error_return (0,
				 "setting rss queues is not supported on this interface");
    }

  if (!error)
    {
      clib_bitmap_free (hi->rss_queues);
      hi->rss_queues = clib_bitmap_dup (bitmap);
    }

done:
  if (error)
    log_err ("hw_set_rss_queues: %U", format_clib_error, error);
  return error;
}

int collect_detailed_interface_stats_flag = 0;

void
collect_detailed_interface_stats_flag_set (void)
{
  collect_detailed_interface_stats_flag = 1;
}

void
collect_detailed_interface_stats_flag_clear (void)
{
  collect_detailed_interface_stats_flag = 0;
}

static clib_error_t *
collect_detailed_interface_stats_cli (vlib_main_t * vm,
				      unformat_input_t * input,
				      vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *error = NULL;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return clib_error_return (0, "expected enable | disable");

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "enable") || unformat (line_input, "on"))
	collect_detailed_interface_stats_flag_set ();
      else if (unformat (line_input, "disable")
	       || unformat (line_input, "off"))
	collect_detailed_interface_stats_flag_clear ();
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

done:
  unformat_free (line_input);
  return error;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (collect_detailed_interface_stats_command, static) = {
  .path = "interface collect detailed-stats",
  .short_help = "interface collect detailed-stats <enable|disable>",
  .function = collect_detailed_interface_stats_cli,
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
