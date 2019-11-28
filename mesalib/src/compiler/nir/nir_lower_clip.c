/*
 * Copyright © 2015 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "nir.h"
#include "nir_builder.h"

#define MAX_CLIP_PLANES 8

/* Generates the lowering code for user-clip-planes, generating CLIPDIST
 * from UCP[n] + CLIPVERTEX or POSITION.  Additionally, an optional pass
 * for fragment shaders to insert conditional kills based on the inter-
 * polated CLIPDIST
 *
 * NOTE: should be run after nir_lower_outputs_to_temporaries() (or at
 * least in scenarios where you can count on each output written once
 * and only once).
 */


static nir_variable *
create_clipdist_var(nir_shader *shader, unsigned drvloc,
                    bool output, gl_varying_slot slot, unsigned array_size)
{
   nir_variable *var = rzalloc(shader, nir_variable);

   var->data.driver_location = drvloc;
   var->data.mode = output ? nir_var_shader_out : nir_var_shader_in;
   var->name = ralloc_asprintf(var, "clipdist_%d", drvloc);
   var->data.index = 0;
   var->data.location = slot;

   if (array_size > 0) {
      var->type = glsl_array_type(glsl_float_type(), array_size,
                                  sizeof(float));
      var->data.compact = 1;
   } else
      var->type = glsl_vec4_type();

   if (output) {
      exec_list_push_tail(&shader->outputs, &var->node);
      shader->num_outputs++; /* TODO use type_size() */
   }
   else {
      exec_list_push_tail(&shader->inputs, &var->node);
      shader->num_inputs++;  /* TODO use type_size() */
   }
   return var;
}

static void
create_clipdist_vars(nir_shader *shader, nir_variable **io_vars,
                     unsigned ucp_enables, int *drvloc, bool output,
                     bool use_clipdist_array)
{
   if (use_clipdist_array) {
      io_vars[0] =
         create_clipdist_var(shader, ++(*drvloc), true,
                             VARYING_SLOT_CLIP_DIST0,
                             util_last_bit(ucp_enables));
   } else {
      if (ucp_enables & 0x0f)
         io_vars[0] =
            create_clipdist_var(shader, ++(*drvloc), output,
                                VARYING_SLOT_CLIP_DIST0, 0);
      if (ucp_enables & 0xf0)
         io_vars[1] =
            create_clipdist_var(shader, ++(*drvloc), output,
                                VARYING_SLOT_CLIP_DIST1, 0);
   }
}

static void
store_clipdist_output(nir_builder *b, nir_variable *out, nir_ssa_def **val)
{
   nir_intrinsic_instr *store;

   store = nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_output);
   store->num_components = 4;
   nir_intrinsic_set_base(store, out->data.driver_location);
   nir_intrinsic_set_write_mask(store, 0xf);
   store->src[0].ssa = nir_vec4(b, val[0], val[1], val[2], val[3]);
   store->src[0].is_ssa = true;
   store->src[1] = nir_src_for_ssa(nir_imm_int(b, 0));
   nir_builder_instr_insert(b, &store->instr);
}

static void
load_clipdist_input(nir_builder *b, nir_variable *in, nir_ssa_def **val)
{
   nir_intrinsic_instr *load;

   load = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_input);
   load->num_components = 4;
   nir_intrinsic_set_base(load, in->data.driver_location);
   load->src[0] = nir_src_for_ssa(nir_imm_int(b, 0));
   nir_ssa_dest_init(&load->instr, &load->dest, 4, 32, NULL);
   nir_builder_instr_insert(b, &load->instr);

   val[0] = nir_channel(b, &load->dest.ssa, 0);
   val[1] = nir_channel(b, &load->dest.ssa, 1);
   val[2] = nir_channel(b, &load->dest.ssa, 2);
   val[3] = nir_channel(b, &load->dest.ssa, 3);
}

static nir_ssa_def *
find_output_in_block(nir_block *block, unsigned drvloc)
{
   nir_foreach_instr(instr, block) {

      if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if ((intr->intrinsic == nir_intrinsic_store_output) &&
             nir_intrinsic_base(intr) == drvloc) {
            assert(intr->src[0].is_ssa);
            assert(nir_src_is_const(intr->src[1]));
            return intr->src[0].ssa;
         }
      }
   }

   return NULL;
}

/* TODO: maybe this would be a useful helper?
 * NOTE: assumes each output is written exactly once (and unconditionally)
 * so if needed nir_lower_outputs_to_temporaries()
 */
static nir_ssa_def *
find_output(nir_shader *shader, unsigned drvloc)
{
   nir_ssa_def *def = NULL;
   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_foreach_block_reverse(block, function->impl) {
            nir_ssa_def *new_def = find_output_in_block(block, drvloc);
            assert(!(new_def && def));
            def = new_def;
#if !defined(DEBUG)
            /* for debug builds, scan entire shader to assert
             * if output is written multiple times.  For release
             * builds just assume all is well and bail when we
             * find first:
             */
            if (def)
               break;
#endif
         }
      }
   }

   return def;
}

static bool
find_clipvertex_and_position_outputs(nir_shader *shader,
                                     nir_variable **clipvertex,
                                     nir_variable **position)
{
   nir_foreach_variable(var, &shader->outputs) {
      switch (var->data.location) {
      case VARYING_SLOT_POS:
         *position = var;
         break;
      case VARYING_SLOT_CLIP_VERTEX:
         *clipvertex = var;
         break;
      case VARYING_SLOT_CLIP_DIST0:
      case VARYING_SLOT_CLIP_DIST1:
         /* if shader is already writing CLIPDIST, then
          * there should be no user-clip-planes to deal
          * with.
          *
          * We assume nir_remove_dead_variables has removed the clipdist
          * variables if they're not written.
          */
         return false;
      }
   }

   return *clipvertex || *position;
}

static nir_ssa_def *
get_ucp(nir_builder *b, int plane,
        const gl_state_index16 clipplane_state_tokens[][STATE_LENGTH])
{
   if (clipplane_state_tokens) {
      char tmp[100];
      snprintf(tmp, ARRAY_SIZE(tmp), "gl_ClipPlane%dMESA", plane);
      nir_variable *var = nir_variable_create(b->shader,
                                              nir_var_uniform,
                                              glsl_vec4_type(),
                                              tmp);

      var->num_state_slots = 1;
      var->state_slots = ralloc_array(var, nir_state_slot, 1);
      memcpy(var->state_slots[0].tokens,
             clipplane_state_tokens[plane],
             sizeof(var->state_slots[0].tokens));
      return nir_load_var(b, var);
   } else
      return nir_load_user_clip_plane(b, plane);
}


static void
lower_clip_outputs(nir_builder *b, nir_variable *position,
                   nir_variable *clipvertex, nir_variable **out,
                   unsigned ucp_enables, bool use_vars,
                   bool use_clipdist_array,
                   const gl_state_index16 clipplane_state_tokens[][STATE_LENGTH])
{
   nir_ssa_def *clipdist[MAX_CLIP_PLANES];
   nir_ssa_def *cv;

   if (use_vars) {
      cv = nir_load_var(b, clipvertex ? clipvertex : position);

      if (clipvertex) {
         exec_node_remove(&clipvertex->node);
         clipvertex->data.mode = nir_var_shader_temp;
         exec_list_push_tail(&b->shader->globals, &clipvertex->node);
         nir_fixup_deref_modes(b->shader);
      }
   } else {
      if (clipvertex)
         cv = find_output(b->shader, clipvertex->data.driver_location);
      else {
         assert(position);
         cv = find_output(b->shader, position->data.driver_location);
      }
   }

   for (int plane = 0; plane < MAX_CLIP_PLANES; plane++) {
      if (ucp_enables & (1 << plane)) {
         nir_ssa_def *ucp = get_ucp(b, plane, clipplane_state_tokens);

         /* calculate clipdist[plane] - dot(ucp, cv): */
         clipdist[plane] = nir_fdot4(b, ucp, cv);
      } else {
         /* 0.0 == don't-clip == disabled: */
         clipdist[plane] = nir_imm_float(b, 0.0);
      }
      if (use_clipdist_array && plane < util_last_bit(ucp_enables)) {
         assert(use_vars);
         nir_deref_instr *deref;
         deref = nir_build_deref_array_imm(b,
                                           nir_build_deref_var(b, out[0]),
                                           plane);
         nir_store_deref(b, deref, clipdist[plane], 1);
      }
   }

   if (!use_clipdist_array) {
      if (use_vars) {
         if (ucp_enables & 0x0f)
            nir_store_var(b, out[0], nir_vec(b, clipdist, 4), 0xf);
         if (ucp_enables & 0xf0)
            nir_store_var(b, out[1], nir_vec(b, &clipdist[4], 4), 0xf);
      } else {
         if (ucp_enables & 0x0f)
            store_clipdist_output(b, out[0], &clipdist[0]);
         if (ucp_enables & 0xf0)
            store_clipdist_output(b, out[1], &clipdist[4]);
      }
   }
}

/*
 * VS lowering
 */

/* ucp_enables is bitmask of enabled ucps.  Actual ucp values are
 * passed in to shader via user_clip_plane system-values
 *
 * If use_vars is true, the pass will use variable loads and stores instead
 * of working with store_output intrinsics.
 *
 * If use_clipdist_array is true, the pass will use compact arrays for the
 * clipdist output instead of two vec4s.
 */
bool
nir_lower_clip_vs(nir_shader *shader, unsigned ucp_enables, bool use_vars,
                  bool use_clipdist_array,
                  const gl_state_index16 clipplane_state_tokens[][STATE_LENGTH])
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_builder b;
   int maxloc = -1;
   nir_variable *position = NULL;
   nir_variable *clipvertex = NULL;
   nir_variable *out[2] = { NULL };

   if (!ucp_enables)
      return false;

   nir_builder_init(&b, impl);

   /* NIR should ensure that, even in case of loops/if-else, there
    * should be only a single predecessor block to end_block, which
    * makes the perfect place to insert the clipdist calculations.
    *
    * NOTE: in case of early returns, these would have to be lowered
    * to jumps to end_block predecessor in a previous pass.  Not sure
    * if there is a good way to sanity check this, but for now the
    * users of this pass don't support sub-routines.
    */
   assert(impl->end_block->predecessors->entries == 1);
   b.cursor = nir_after_cf_list(&impl->body);

   /* find clipvertex/position outputs */
   if (!find_clipvertex_and_position_outputs(shader, &clipvertex, &position))
      return false;

   /* insert CLIPDIST outputs */
   create_clipdist_vars(shader, out, ucp_enables, &maxloc, true,
                        use_clipdist_array);

   lower_clip_outputs(&b, position, clipvertex, out, ucp_enables, use_vars,
                      use_clipdist_array, clipplane_state_tokens);

   nir_metadata_preserve(impl, nir_metadata_dominance);

   return true;
}

static void
lower_clip_in_gs_block(nir_builder *b, nir_block *block, nir_variable *position,
                       nir_variable *clipvertex, nir_variable **out,
                       unsigned ucp_enables, bool use_clipdist_array,
                       const gl_state_index16 clipplane_state_tokens[][STATE_LENGTH])
{
   nir_foreach_instr_safe(instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_emit_vertex_with_counter:
      case nir_intrinsic_emit_vertex:
         b->cursor = nir_before_instr(instr);
         lower_clip_outputs(b, position, clipvertex, out, ucp_enables, true,
                            use_clipdist_array, clipplane_state_tokens);
         break;
      default:
         /* not interesting; skip this */
         break;
      }
   }
}

/*
 * GS lowering
 */

bool
nir_lower_clip_gs(nir_shader *shader, unsigned ucp_enables,
                  bool use_clipdist_array,
                  const gl_state_index16 clipplane_state_tokens[][STATE_LENGTH])
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   nir_builder b;
   int maxloc = -1;
   nir_variable *position = NULL;
   nir_variable *clipvertex = NULL;
   nir_variable *out[2] = { NULL };

   if (!ucp_enables)
      return false;

   /* find clipvertex/position outputs */
   if (!find_clipvertex_and_position_outputs(shader, &clipvertex, &position))
      return false;

   /* insert CLIPDIST outputs */
   create_clipdist_vars(shader, out, ucp_enables, &maxloc, true,
                        use_clipdist_array);

   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl)
      lower_clip_in_gs_block(&b, block, position, clipvertex, out,
                             ucp_enables, use_clipdist_array,
                             clipplane_state_tokens);

   nir_metadata_preserve(impl, nir_metadata_dominance);

   return true;
}

/*
 * FS lowering
 */

static void
lower_clip_fs(nir_function_impl *impl, unsigned ucp_enables,
              nir_variable **in)
{
   nir_ssa_def *clipdist[MAX_CLIP_PLANES];
   nir_builder b;

   nir_builder_init(&b, impl);
   b.cursor = nir_before_cf_list(&impl->body);

   if (ucp_enables & 0x0f)
      load_clipdist_input(&b, in[0], &clipdist[0]);
   if (ucp_enables & 0xf0)
      load_clipdist_input(&b, in[1], &clipdist[4]);

   for (int plane = 0; plane < MAX_CLIP_PLANES; plane++) {
      if (ucp_enables & (1 << plane)) {
         nir_intrinsic_instr *discard;
         nir_ssa_def *cond;

         cond = nir_flt(&b, clipdist[plane], nir_imm_float(&b, 0.0));

         discard = nir_intrinsic_instr_create(b.shader,
                                              nir_intrinsic_discard_if);
         discard->src[0] = nir_src_for_ssa(cond);
         nir_builder_instr_insert(&b, &discard->instr);

         b.shader->info.fs.uses_discard = true;
      }
   }

   nir_metadata_preserve(impl, nir_metadata_dominance);
}

/* insert conditional kill based on interpolated CLIPDIST
 */
bool
nir_lower_clip_fs(nir_shader *shader, unsigned ucp_enables,
                  bool use_clipdist_array)
{
   nir_variable *in[2];
   int maxloc = -1;

   if (!ucp_enables)
      return false;

   nir_foreach_variable(var, &shader->inputs) {
      int loc = var->data.driver_location;

      /* keep track of last used driver-location.. we'll be
       * appending CLIP_DIST0/CLIP_DIST1 after last existing
       * input:
       */
      maxloc = MAX2(maxloc, loc);
   }

   /* The shader won't normally have CLIPDIST inputs, so we
    * must add our own:
    */
   /* insert CLIPDIST inputs */
   create_clipdist_vars(shader, in, ucp_enables, &maxloc, false,
                        use_clipdist_array);

   nir_foreach_function(function, shader) {
      if (!strcmp(function->name, "main"))
         lower_clip_fs(function->impl, ucp_enables, in);
   }

   return true;
}
