/*
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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup modifiers
 */

#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_action.h" /* BKE_pose_channel_find_name */
#include "BKE_colortools.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_texture.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "RE_shader_ext.h"

#include "MOD_util.h"

static void initData(ModifierData *md)
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  wmd->curfalloff = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  wmd->texture = NULL;
  wmd->strength = 1.0f;
  wmd->falloff_radius = 1.0f;
  wmd->falloff_type = eWarp_Falloff_Smooth;
  wmd->flag = 0;
}

static void copyData(const ModifierData *md, ModifierData *target, const int flag)
{
  const WarpModifierData *wmd = (const WarpModifierData *)md;
  WarpModifierData *twmd = (WarpModifierData *)target;

  modifier_copyData_generic(md, target, flag);

  twmd->curfalloff = BKE_curvemapping_copy(wmd->curfalloff);
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  /* ask for vertexgroups if we need them */
  if (wmd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }

  /* ask for UV coordinates if we need them */
  if (wmd->texmapping == MOD_DISP_MAP_UV) {
    r_cddata_masks->fmask |= CD_MASK_MTFACE;
  }
}

static void matrix_from_obj_pchan(float mat[4][4],
                                  const float obinv[4][4],
                                  Object *ob,
                                  const char *bonename)
{
  bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, bonename);
  if (pchan) {
    float mat_bone_world[4][4];
    mul_m4_m4m4(mat_bone_world, ob->obmat, pchan->pose_mat);
    mul_m4_m4m4(mat, obinv, mat_bone_world);
  }
  else {
    mul_m4_m4m4(mat, obinv, ob->obmat);
  }
}

static bool dependsOnTime(ModifierData *md)
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  if (wmd->texture) {
    return BKE_texture_dependsOnTime(wmd->texture);
  }
  else {
    return false;
  }
}

static void freeData(ModifierData *md)
{
  WarpModifierData *wmd = (WarpModifierData *)md;
  BKE_curvemapping_free(wmd->curfalloff);
}

static bool isDisabled(const struct Scene *UNUSED(scene),
                       ModifierData *md,
                       bool UNUSED(userRenderParams))
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  return !(wmd->object_from && wmd->object_to);
}

static void foreachObjectLink(ModifierData *md, Object *ob, ObjectWalkFunc walk, void *userData)
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  walk(userData, ob, &wmd->object_from, IDWALK_CB_NOP);
  walk(userData, ob, &wmd->object_to, IDWALK_CB_NOP);
  walk(userData, ob, &wmd->map_object, IDWALK_CB_NOP);
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  walk(userData, ob, (ID **)&wmd->texture, IDWALK_CB_USER);

  foreachObjectLink(md, ob, (ObjectWalkFunc)walk, userData);
}

static void foreachTexLink(ModifierData *md, Object *ob, TexWalkFunc walk, void *userData)
{
  walk(userData, ob, md, "texture");
}

static void warp_deps_object_bone_new(struct DepsNodeHandle *node,
                                      Object *object,
                                      const char *bonename)
{
  if (bonename[0] && object->type == OB_ARMATURE) {
    DEG_add_object_relation(node, object, DEG_OB_COMP_EVAL_POSE, "Warp Modifier");
  }
  else {
    DEG_add_object_relation(node, object, DEG_OB_COMP_TRANSFORM, "Warp Modifier");
  }
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  WarpModifierData *wmd = (WarpModifierData *)md;

  if (wmd->object_from != NULL && wmd->object_to != NULL) {
    warp_deps_object_bone_new(ctx->node, wmd->object_from, wmd->bone_from);
    warp_deps_object_bone_new(ctx->node, wmd->object_to, wmd->bone_to);

    DEG_add_modifier_to_transform_relation(ctx->node, "Warp Modifier");
  }

  if ((wmd->texmapping == MOD_DISP_MAP_OBJECT) && wmd->map_object != NULL) {
    DEG_add_object_relation(
        ctx->node, wmd->map_object, DEG_OB_COMP_TRANSFORM, "Warp Modifier map");
  }
  if (wmd->texture != NULL) {
    DEG_add_generic_id_relation(ctx->node, &wmd->texture->id, "Warp Modifier");
  }
}

static void warpModifier_do(WarpModifierData *wmd,
                            const ModifierEvalContext *ctx,
                            Mesh *mesh,
                            float (*vertexCos)[3],
                            int numVerts)
{
  Object *ob = ctx->object;
  float obinv[4][4];
  float mat_from[4][4];
  float mat_from_inv[4][4];
  float mat_to[4][4];
  float mat_unit[4][4];
  float mat_final[4][4];

  float tmat[4][4];

  const float falloff_radius_sq = square_f(wmd->falloff_radius);
  float strength = wmd->strength;
  float fac = 1.0f, weight;
  int i;
  int defgrp_index;
  MDeformVert *dvert, *dv = NULL;
  const bool invert_vgroup = (wmd->flag & MOD_WARP_INVERT_VGROUP) != 0;
  float(*tex_co)[3] = NULL;

  if (!(wmd->object_from && wmd->object_to)) {
    return;
  }

  MOD_get_vgroup(ob, mesh, wmd->defgrp_name, &dvert, &defgrp_index);
  if (dvert == NULL) {
    defgrp_index = -1;
  }

  if (wmd->curfalloff == NULL) { /* should never happen, but bad lib linking could cause it */
    wmd->curfalloff = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  }

  if (wmd->curfalloff) {
    BKE_curvemapping_initialize(wmd->curfalloff);
  }

  invert_m4_m4(obinv, ob->obmat);

  /* Checks that the objects/bones are available. */
  matrix_from_obj_pchan(mat_from, obinv, wmd->object_from, wmd->bone_from);
  matrix_from_obj_pchan(mat_to, obinv, wmd->object_to, wmd->bone_to);

  invert_m4_m4(tmat, mat_from);  // swap?
  mul_m4_m4m4(mat_final, tmat, mat_to);

  invert_m4_m4(mat_from_inv, mat_from);

  unit_m4(mat_unit);

  if (strength < 0.0f) {
    float loc[3];
    strength = -strength;

    /* inverted location is not useful, just use the negative */
    copy_v3_v3(loc, mat_final[3]);
    invert_m4(mat_final);
    negate_v3_v3(mat_final[3], loc);
  }
  weight = strength;

  Tex *tex_target = wmd->texture;
  if (mesh != NULL && tex_target != NULL) {
    tex_co = MEM_malloc_arrayN(numVerts, sizeof(*tex_co), "warpModifier_do tex_co");
    MOD_get_texture_coords((MappingInfoModifierData *)wmd, ctx, ob, mesh, vertexCos, tex_co);

    MOD_init_texture((MappingInfoModifierData *)wmd, ctx);
  }

  for (i = 0; i < numVerts; i++) {
    float *co = vertexCos[i];

    if (wmd->falloff_type == eWarp_Falloff_None ||
        ((fac = len_squared_v3v3(co, mat_from[3])) < falloff_radius_sq &&
         (fac = (wmd->falloff_radius - sqrtf(fac)) / wmd->falloff_radius))) {
      /* skip if no vert group found */
      if (defgrp_index != -1) {
        dv = &dvert[i];
        weight = invert_vgroup ? 1.0f - BKE_defvert_find_weight(dv, defgrp_index) * strength :
                                 BKE_defvert_find_weight(dv, defgrp_index) * strength;
        if (weight <= 0.0f) {
          continue;
        }
      }

      /* closely match PROP_SMOOTH and similar */
      switch (wmd->falloff_type) {
        case eWarp_Falloff_None:
          fac = 1.0f;
          break;
        case eWarp_Falloff_Curve:
          fac = BKE_curvemapping_evaluateF(wmd->curfalloff, 0, fac);
          break;
        case eWarp_Falloff_Sharp:
          fac = fac * fac;
          break;
        case eWarp_Falloff_Smooth:
          fac = 3.0f * fac * fac - 2.0f * fac * fac * fac;
          break;
        case eWarp_Falloff_Root:
          fac = sqrtf(fac);
          break;
        case eWarp_Falloff_Linear:
          /* pass */
          break;
        case eWarp_Falloff_Const:
          fac = 1.0f;
          break;
        case eWarp_Falloff_Sphere:
          fac = sqrtf(2 * fac - fac * fac);
          break;
        case eWarp_Falloff_InvSquare:
          fac = fac * (2.0f - fac);
          break;
      }

      fac *= weight;

      if (tex_co) {
        struct Scene *scene = DEG_get_evaluated_scene(ctx->depsgraph);
        TexResult texres;
        texres.nor = NULL;
        BKE_texture_get_value(scene, tex_target, tex_co[i], &texres, false);
        fac *= texres.tin;
      }

      if (fac != 0.0f) {
        /* into the 'from' objects space */
        mul_m4_v3(mat_from_inv, co);

        if (fac == 1.0f) {
          mul_m4_v3(mat_final, co);
        }
        else {
          if (wmd->flag & MOD_WARP_VOLUME_PRESERVE) {
            /* interpolate the matrix for nicer locations */
            blend_m4_m4m4(tmat, mat_unit, mat_final, fac);
            mul_m4_v3(tmat, co);
          }
          else {
            float tvec[3];
            mul_v3_m4v3(tvec, mat_final, co);
            interp_v3_v3v3(co, co, tvec, fac);
          }
        }

        /* out of the 'from' objects space */
        mul_m4_v3(mat_from, co);
      }
    }
  }

  if (tex_co) {
    MEM_freeN(tex_co);
  }
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int numVerts)
{
  WarpModifierData *wmd = (WarpModifierData *)md;
  Mesh *mesh_src = NULL;

  if (wmd->defgrp_name[0] != '\0' || wmd->texture != NULL) {
    /* mesh_src is only needed for vgroups and textures. */
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, numVerts, false, false);
  }

  warpModifier_do(wmd, ctx, mesh_src, vertexCos, numVerts);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void deformVertsEM(ModifierData *md,
                          const ModifierEvalContext *ctx,
                          struct BMEditMesh *em,
                          Mesh *mesh,
                          float (*vertexCos)[3],
                          int numVerts)
{
  WarpModifierData *wmd = (WarpModifierData *)md;
  Mesh *mesh_src = NULL;

  if (wmd->defgrp_name[0] != '\0' || wmd->texture != NULL) {
    /* mesh_src is only needed for vgroups and textures. */
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, em, mesh, NULL, numVerts, false, false);
  }

  warpModifier_do(wmd, ctx, mesh_src, vertexCos, numVerts);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

ModifierTypeInfo modifierType_Warp = {
    /* name */ "Warp",
    /* structName */ "WarpModifierData",
    /* structSize */ sizeof(WarpModifierData),
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsCVs | eModifierTypeFlag_AcceptsLattice |
        eModifierTypeFlag_SupportsEditmode,
    /* copyData */ copyData,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ deformVertsEM,
    /* deformMatricesEM */ NULL,
    /* applyModifier */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ freeData,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ NULL,
    /* foreachObjectLink */ foreachObjectLink,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ foreachTexLink,
    /* freeRuntimeData */ NULL,
};
