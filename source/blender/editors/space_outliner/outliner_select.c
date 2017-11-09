/*
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2004 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Joshua Leung
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/space_outliner/outliner_select.c
 *  \ingroup spoutliner
 */

#include <stdlib.h>

#include "DNA_armature_types.h"
#include "DNA_group_types.h"
#include "DNA_lamp_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_world_types.h"
#include "DNA_gpencil_types.h"

#include "BLI_utildefines.h"
#include "BLI_listbase.h"

#include "BKE_context.h"
#include "BKE_object.h"
#include "BKE_layer.h"
#include "BKE_scene.h"
#include "BKE_sequencer.h"
#include "BKE_armature.h"
#include "BKE_workspace.h"
#include "BKE_paint.h"

#include "DEG_depsgraph.h"

#include "ED_armature.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sequencer.h"
#include "ED_util.h"
#include "ED_gpencil.h"

#include "WM_api.h"
#include "WM_types.h"


#include "UI_interface.h"
#include "UI_view2d.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "outliner_intern.h"


/* ****************************************************** */
/* Outliner Element Selection/Activation on Click */

static eOLDrawState tree_element_active_renderlayer(
        bContext *C, Scene *UNUSED(scene), SceneLayer *UNUSED(sl), TreeElement *te, TreeStoreElem *tselem, const eOLSetState set)
{
	Scene *sce;
	
	/* paranoia check */
	if (te->idcode != ID_SCE)
		return OL_DRAWSEL_NONE;
	sce = (Scene *)tselem->id;
	
	if (set != OL_SETSEL_NONE) {
		sce->r.actlay = tselem->nr;
		WM_event_add_notifier(C, NC_SCENE | ND_RENDER_OPTIONS, sce);
	}
	else {
		return sce->r.actlay == tselem->nr;
	}
	return OL_DRAWSEL_NONE;
}

/**
 * Select object tree:
 * CTRL+LMB: Select/Deselect object and all cildren
 * CTRL+SHIFT+LMB: Add/Remove object and all children
 */
static void do_outliner_object_select_recursive(SceneLayer *sl, Object *ob_parent, bool select)
{
	Base *base;

	for (base = FIRSTBASE(sl); base; base = base->next) {
		Object *ob = base->object;
		if ((((base->flag & BASE_VISIBLED) == 0) && BKE_object_is_child_recursive(ob_parent, ob))) {
			ED_object_base_select(base, select ? BA_SELECT : BA_DESELECT);
		}
	}
}

static void do_outliner_bone_select_recursive(bArmature *arm, Bone *bone_parent, bool select)
{
	Bone *bone;
	for (bone = bone_parent->childbase.first; bone; bone = bone->next) {
		if (select && PBONE_SELECTABLE(arm, bone))
			bone->flag |= BONE_SELECTED;
		else
			bone->flag &= ~(BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL);
		do_outliner_bone_select_recursive(arm, bone, select);
	}
}

static void do_outliner_ebone_select_recursive(bArmature *arm, EditBone *ebone_parent, bool select)
{
	EditBone *ebone;
	for (ebone = ebone_parent->next; ebone; ebone = ebone->next) {
		if (ED_armature_ebone_is_child_recursive(ebone_parent, ebone)) {
			if (select && EBONE_SELECTABLE(arm, ebone))
				ebone->flag |= BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL;
			else
				ebone->flag &= ~(BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL);
		}
	}
}

static eOLDrawState tree_element_set_active_object(
        bContext *C, Scene *scene, SceneLayer *sl, SpaceOops *soops,
        TreeElement *te, const eOLSetState set, bool recursive)
{
	TreeStoreElem *tselem = TREESTORE(te);
	Scene *sce;
	Base *base;
	Object *ob = NULL;
	
	/* if id is not object, we search back */
	if (te->idcode == ID_OB) {
		ob = (Object *)tselem->id;
	}
	else {
		ob = (Object *)outliner_search_back(soops, te, ID_OB);
		if (ob == OBACT(sl)) {
			return OL_DRAWSEL_NONE;
		}
	}
	if (ob == NULL) {
		return OL_DRAWSEL_NONE;
	}
	
	sce = (Scene *)outliner_search_back(soops, te, ID_SCE);
	if (sce && scene != sce) {
		WM_window_change_active_scene(CTX_data_main(C), C, CTX_wm_window(C), sce);
		scene = sce;
	}
	
	/* find associated base in current scene */
	base = BKE_scene_layer_base_find(sl, ob);

	if (base) {
		if (set == OL_SETSEL_EXTEND) {
			/* swap select */
			if (base->flag & BASE_SELECTED)
				ED_object_base_select(base, BA_DESELECT);
			else 
				ED_object_base_select(base, BA_SELECT);
		}
		else {
			/* deleselect all */
			BKE_scene_layer_base_deselect_all(sl);
			ED_object_base_select(base, BA_SELECT);
		}

		if (recursive) {
			/* Recursive select/deselect for Object hierarchies */
			do_outliner_object_select_recursive(sl, ob, (ob->flag & SELECT) != 0);
		}

		if (set != OL_SETSEL_NONE) {
			ED_object_base_activate(C, base); /* adds notifier */
			WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
		}
	}
	
	/* set special grease pencil modes */
	if (ob->type == OB_GPENCIL) {
		/* set cursor */
		if (ob->mode == OB_MODE_GPENCIL_PAINT) {
			WM_cursor_modal_set(CTX_wm_window(C), BC_PAINTBRUSHCURSOR);
			ED_gpencil_toggle_brush_cursor(C, false);
		}
		else if (ob->mode == OB_MODE_GPENCIL_SCULPT) {
			WM_cursor_modal_set(CTX_wm_window(C), BC_CROSSCURSOR);
			ED_gpencil_toggle_brush_cursor(C, true);
		}
		else if (ob->mode == OB_MODE_GPENCIL_WEIGHT) {
			WM_cursor_modal_set(CTX_wm_window(C), BC_CROSSCURSOR);
			ED_gpencil_toggle_brush_cursor(C, true);
		}
		else {
			WM_cursor_modal_set(CTX_wm_window(C), CURSOR_STD);
			ED_gpencil_toggle_brush_cursor(C, false);
		}
		/* set workspace mode */
		BKE_workspace_object_mode_set(CTX_wm_workspace(C), ob->mode);
	}

	if (ob != scene->obedit)
		ED_object_editmode_exit(C, EM_FREEDATA | EM_FREEUNDO | EM_WAITCURSOR | EM_DO_UNDO);
		
	return OL_DRAWSEL_NORMAL;
}

static eOLDrawState tree_element_active_material(
        bContext *C, Scene *UNUSED(scene), SceneLayer *sl, SpaceOops *soops,
        TreeElement *te, const eOLSetState set)
{
	TreeElement *tes;
	Object *ob;
	
	/* we search for the object parent */
	ob = (Object *)outliner_search_back(soops, te, ID_OB);
	// note: ob->matbits can be NULL when a local object points to a library mesh.
	if (ob == NULL || ob != OBACT(sl) || ob->matbits == NULL) {
		return OL_DRAWSEL_NONE;  /* just paranoia */
	}
	
	/* searching in ob mat array? */
	tes = te->parent;
	if (tes->idcode == ID_OB) {
		if (set != OL_SETSEL_NONE) {
			ob->actcol = te->index + 1;
			ob->matbits[te->index] = 1;  // make ob material active too
		}
		else {
			if (ob->actcol == te->index + 1) {
				if (ob->matbits[te->index]) {
					return OL_DRAWSEL_NORMAL;
				}
			}
		}
	}
	/* or we search for obdata material */
	else {
		if (set != OL_SETSEL_NONE) {
			ob->actcol = te->index + 1;
			ob->matbits[te->index] = 0;  // make obdata material active too
		}
		else {
			if (ob->actcol == te->index + 1) {
				if (ob->matbits[te->index] == 0) {
					return OL_DRAWSEL_NORMAL;
				}
			}
		}
	}
	if (set != OL_SETSEL_NONE) {
		/* Tagging object for update seems a bit stupid here, but looks like we have to do it
		 * for render views to update. See T42973.
		 * Note that RNA material update does it too, see e.g. rna_MaterialSlot_update(). */
		DEG_id_tag_update((ID *)ob, OB_RECALC_OB);
		WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING_LINKS, NULL);
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_texture(
        bContext *C, Scene *scene, SceneLayer *sl, SpaceOops *UNUSED(soops),
        TreeElement *te, const eOLSetState set)
{
	TreeElement *tep;
	TreeStoreElem /* *tselem,*/ *tselemp;
	Object *ob = OBACT(sl);
	SpaceButs *sbuts = NULL;
	
	if (ob == NULL) {
		/* no active object */
		return OL_DRAWSEL_NONE;
	}
	
	/*tselem = TREESTORE(te);*/ /*UNUSED*/
	
	/* find buttons region (note, this is undefined really still, needs recode in blender) */
	/* XXX removed finding sbuts */
	
	/* where is texture linked to? */
	tep = te->parent;
	tselemp = TREESTORE(tep);
	
	if (tep->idcode == ID_WO) {
		World *wrld = (World *)tselemp->id;

		if (set != OL_SETSEL_NONE) {
			if (sbuts) {
				// XXX sbuts->tabo = TAB_SHADING_TEX;	// hack from header_buttonswin.c
				// XXX sbuts->texfrom = 1;
			}
// XXX			extern_set_butspace(F6KEY, 0);	// force shading buttons texture
			wrld->texact = te->index;
		}
		else if (tselemp->id == (ID *)(scene->world)) {
			if (wrld->texact == te->index) {
				return OL_DRAWSEL_NORMAL;
			}
		}
	}
	else if (tep->idcode == ID_LA) {
		Lamp *la = (Lamp *)tselemp->id;
		if (set != OL_SETSEL_NONE) {
			if (sbuts) {
				// XXX sbuts->tabo = TAB_SHADING_TEX;	// hack from header_buttonswin.c
				// XXX sbuts->texfrom = 2;
			}
// XXX			extern_set_butspace(F6KEY, 0);	// force shading buttons texture
			la->texact = te->index;
		}
		else {
			if (tselemp->id == ob->data) {
				if (la->texact == te->index) {
					return OL_DRAWSEL_NORMAL;
				}
			}
		}
	}
	else if (tep->idcode == ID_MA) {
		Material *ma = (Material *)tselemp->id;
		if (set != OL_SETSEL_NONE) {
			if (sbuts) {
				//sbuts->tabo = TAB_SHADING_TEX;	// hack from header_buttonswin.c
				// XXX sbuts->texfrom = 0;
			}
// XXX			extern_set_butspace(F6KEY, 0);	// force shading buttons texture
			ma->texact = (char)te->index;
			
			/* also set active material */
			ob->actcol = tep->index + 1;
		}
		else if (tep->flag & TE_ACTIVE) {   // this is active material
			if (ma->texact == te->index) {
				return OL_DRAWSEL_NORMAL;
			}
		}
	}
	
	if (set != OL_SETSEL_NONE) {
		WM_event_add_notifier(C, NC_TEXTURE, NULL);
	}

	/* no active object */
	return OL_DRAWSEL_NONE;
}


static eOLDrawState tree_element_active_lamp(
        bContext *UNUSED(C), Scene *UNUSED(scene), SceneLayer *sl, SpaceOops *soops,
        TreeElement *te, const eOLSetState set)
{
	Object *ob;
	
	/* we search for the object parent */
	ob = (Object *)outliner_search_back(soops, te, ID_OB);
	if (ob == NULL || ob != OBACT(sl)) {
		/* just paranoia */
		return OL_DRAWSEL_NONE;
	}
	
	if (set != OL_SETSEL_NONE) {
// XXX		extern_set_butspace(F5KEY, 0);
	}
	else {
		return OL_DRAWSEL_NORMAL;
	}
	
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_camera(
        bContext *UNUSED(C), Scene *scene, SceneLayer *UNUSED(sl), SpaceOops *soops,
        TreeElement *te, const eOLSetState set)
{
	Object *ob = (Object *)outliner_search_back(soops, te, ID_OB);

	if (set != OL_SETSEL_NONE) {
		return OL_DRAWSEL_NONE;
	}

	return scene->camera == ob;
}

static eOLDrawState tree_element_active_world(
        bContext *C, Scene *scene, SceneLayer *UNUSED(sl), SpaceOops *UNUSED(soops),
        TreeElement *te, const eOLSetState set)
{
	TreeElement *tep;
	TreeStoreElem *tselem = NULL;
	Scene *sce = NULL;
	
	tep = te->parent;
	if (tep) {
		tselem = TREESTORE(tep);
		if (tselem->type == 0)
			sce = (Scene *)tselem->id;
	}
	
	if (set != OL_SETSEL_NONE) {
		/* make new scene active */
		if (sce && scene != sce) {
			WM_window_change_active_scene(CTX_data_main(C), C, CTX_wm_window(C), sce);
		}
	}
	
	if (tep == NULL || tselem->id == (ID *)scene) {
		if (set != OL_SETSEL_NONE) {
// XXX			extern_set_butspace(F8KEY, 0);
		}
		else {
			return OL_DRAWSEL_NORMAL;
		}
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_defgroup(
        bContext *C, SceneLayer *sl, TreeElement *te, TreeStoreElem *tselem, const eOLSetState set)
{
	Object *ob;
	
	/* id in tselem is object */
	ob = (Object *)tselem->id;
	if (set != OL_SETSEL_NONE) {
		BLI_assert(te->index + 1 >= 0);
		ob->actdef = te->index + 1;

		DEG_id_tag_update(&ob->id, OB_RECALC_DATA);
		WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, ob);
	}
	else {
		if (ob == OBACT(sl))
			if (ob->actdef == te->index + 1) {
				return OL_DRAWSEL_NORMAL;
			}
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_posegroup(
        bContext *C, Scene *UNUSED(scene), SceneLayer *sl, TreeElement *te, TreeStoreElem *tselem, const eOLSetState set)
{
	Object *ob = (Object *)tselem->id;
	
	if (set != OL_SETSEL_NONE) {
		if (ob->pose) {
			ob->pose->active_group = te->index + 1;
			WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
		}
	}
	else {
		if (ob == OBACT(sl) && ob->pose) {
			if (ob->pose->active_group == te->index + 1) {
				return OL_DRAWSEL_NORMAL;
			}
		}
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_posechannel(
        bContext *C, Scene *UNUSED(scene), SceneLayer *sl, TreeElement *te, TreeStoreElem *tselem, const eOLSetState set, bool recursive)
{
	Object *ob = (Object *)tselem->id;
	bArmature *arm = ob->data;
	bPoseChannel *pchan = te->directdata;
	
	if (set != OL_SETSEL_NONE) {
		if (!(pchan->bone->flag & BONE_HIDDEN_P)) {
			
			if (set != OL_SETSEL_EXTEND) {
				bPoseChannel *pchannel;
				/* single select forces all other bones to get unselected */
				for (pchannel = ob->pose->chanbase.first; pchannel; pchannel = pchannel->next)
					pchannel->bone->flag &= ~(BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL);
			}

			if ((set == OL_SETSEL_EXTEND) && (pchan->bone->flag & BONE_SELECTED)) {
				pchan->bone->flag &= ~BONE_SELECTED;
			}
			else {
				pchan->bone->flag |= BONE_SELECTED;
				arm->act_bone = pchan->bone;
			}

			if (recursive) {
				/* Recursive select/deselect */
				do_outliner_bone_select_recursive(arm, pchan->bone, (pchan->bone->flag & BONE_SELECTED) != 0);
			}

			WM_event_add_notifier(C, NC_OBJECT | ND_BONE_ACTIVE, ob);

		}
	}
	else {
		if (ob == OBACT(sl) && ob->pose) {
			if (pchan->bone->flag & BONE_SELECTED) {
				return OL_DRAWSEL_NORMAL;
			}
		}
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_bone(
        bContext *C, SceneLayer *sl, TreeElement *te, TreeStoreElem *tselem, const eOLSetState set, bool recursive)
{
	bArmature *arm = (bArmature *)tselem->id;
	Bone *bone = te->directdata;
	
	if (set != OL_SETSEL_NONE) {
		if (!(bone->flag & BONE_HIDDEN_P)) {
			Object *ob = OBACT(sl);
			if (ob) {
				if (set != OL_SETSEL_EXTEND) {
					/* single select forces all other bones to get unselected */
					for (Bone *bone_iter = arm->bonebase.first; bone_iter != NULL; bone_iter = bone_iter->next) {
						bone_iter->flag &= ~(BONE_TIPSEL | BONE_SELECTED | BONE_ROOTSEL);
						do_outliner_bone_select_recursive(arm, bone_iter, false);
					}
				}
			}
			
			if (set == OL_SETSEL_EXTEND && (bone->flag & BONE_SELECTED)) {
				bone->flag &= ~BONE_SELECTED;
			}
			else {
				bone->flag |= BONE_SELECTED;
				arm->act_bone = bone;
			}

			if (recursive) {
				/* Recursive select/deselect */
				do_outliner_bone_select_recursive(arm, bone, (bone->flag & BONE_SELECTED) != 0);
			}

			
			WM_event_add_notifier(C, NC_OBJECT | ND_BONE_ACTIVE, ob);
		}
	}
	else {
		Object *ob = OBACT(sl);
		
		if (ob && ob->data == arm) {
			if (bone->flag & BONE_SELECTED) {
				return OL_DRAWSEL_NORMAL;
			}
		}
	}
	return OL_DRAWSEL_NONE;
}


/* ebones only draw in editmode armature */
static void tree_element_active_ebone__sel(bContext *C, Scene *scene, bArmature *arm, EditBone *ebone, short sel)
{
	if (sel) {
		ebone->flag |= BONE_SELECTED | BONE_ROOTSEL | BONE_TIPSEL;
		arm->act_edbone = ebone;
		// flush to parent?
		if (ebone->parent && (ebone->flag & BONE_CONNECTED)) ebone->parent->flag |= BONE_TIPSEL;
	}
	else {
		ebone->flag &= ~(BONE_SELECTED | BONE_ROOTSEL | BONE_TIPSEL);
		// flush to parent?
		if (ebone->parent && (ebone->flag & BONE_CONNECTED)) ebone->parent->flag &= ~BONE_TIPSEL;
	}

	WM_event_add_notifier(C, NC_OBJECT | ND_BONE_ACTIVE, scene->obedit);
}
static eOLDrawState tree_element_active_ebone(
        bContext *C, Scene *scene, TreeElement *te, TreeStoreElem *UNUSED(tselem), const eOLSetState set, bool recursive)
{
	BLI_assert(scene->obedit != NULL);

	bArmature *arm = scene->obedit->data;
	EditBone *ebone = te->directdata;
	eOLDrawState status = OL_DRAWSEL_NONE;

	if (set != OL_SETSEL_NONE) {
		if (set == OL_SETSEL_NORMAL) {
			if (!(ebone->flag & BONE_HIDDEN_A)) {
				ED_armature_deselect_all(scene->obedit);
				tree_element_active_ebone__sel(C, scene, arm, ebone, true);
				status = OL_DRAWSEL_NORMAL;
			}
		}
		else if (set == OL_SETSEL_EXTEND) {
			if (!(ebone->flag & BONE_HIDDEN_A)) {
				if (!(ebone->flag & BONE_SELECTED)) {
					tree_element_active_ebone__sel(C, scene, arm, ebone, true);
					status = OL_DRAWSEL_NORMAL;
				}
				else {
					/* entirely selected, so de-select */
					tree_element_active_ebone__sel(C, scene, arm, ebone, false);
					status = OL_DRAWSEL_NONE;
				}
			}
		}

		if (recursive) {
			/* Recursive select/deselect */
			do_outliner_ebone_select_recursive(arm, ebone, (ebone->flag & BONE_SELECTED) != 0);
		}
	}
	else if (ebone->flag & BONE_SELECTED) {
		status = OL_DRAWSEL_NORMAL;
	}

	return status;
}

static eOLDrawState tree_element_active_modifier(
        bContext *C, Scene *UNUSED(scene), SceneLayer *UNUSED(sl), TreeElement *UNUSED(te), TreeStoreElem *tselem, const eOLSetState set)
{
	if (set != OL_SETSEL_NONE) {
		Object *ob = (Object *)tselem->id;
		
		WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

// XXX		extern_set_butspace(F9KEY, 0);
	}
	
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_psys(
        bContext *C, Scene *UNUSED(scene), TreeElement *UNUSED(te), TreeStoreElem *tselem, const eOLSetState set)
{
	if (set != OL_SETSEL_NONE) {
		Object *ob = (Object *)tselem->id;
		
		WM_event_add_notifier(C, NC_OBJECT | ND_PARTICLE | NA_EDITED, ob);
		
// XXX		extern_set_butspace(F7KEY, 0);
	}
	
	return OL_DRAWSEL_NONE;
}

static int tree_element_active_constraint(
        bContext *C, Scene *UNUSED(scene), SceneLayer *UNUSED(sl), TreeElement *UNUSED(te), TreeStoreElem *tselem, const eOLSetState set)
{
	if (set != OL_SETSEL_NONE) {
		Object *ob = (Object *)tselem->id;
		
		WM_event_add_notifier(C, NC_OBJECT | ND_CONSTRAINT, ob);
// XXX		extern_set_butspace(F7KEY, 0);
	}
	
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_text(
        bContext *UNUSED(C), Scene *UNUSED(scene), SceneLayer *UNUSED(sl), SpaceOops *UNUSED(soops),
        TreeElement *UNUSED(te), int UNUSED(set))
{
	// XXX removed
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_pose(
        bContext *C, Scene *scene, SceneLayer *sl, TreeElement *UNUSED(te), TreeStoreElem *tselem, const eOLSetState set)
{
	Object *ob = (Object *)tselem->id;
	Base *base = BKE_scene_layer_base_find(sl, ob);

	if (base == NULL) {
		/* Armature not instantiated in current scene (e.g. inside an appended group...). */
		return OL_DRAWSEL_NONE;
	}

	if (set != OL_SETSEL_NONE) {
		if (scene->obedit)
			ED_object_editmode_exit(C, EM_FREEDATA | EM_FREEUNDO | EM_WAITCURSOR | EM_DO_UNDO);
		
		if (ob->mode & OB_MODE_POSE)
			ED_armature_exit_posemode(C, base);
		else 
			ED_armature_enter_posemode(C, base);
	}
	else {
		if (ob->mode & OB_MODE_POSE) {
			return OL_DRAWSEL_NORMAL;
		}
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_sequence(
        bContext *C, Scene *scene, TreeElement *te, TreeStoreElem *UNUSED(tselem), const eOLSetState set)
{
	Sequence *seq = (Sequence *) te->directdata;
	Editing *ed = BKE_sequencer_editing_get(scene, false);

	if (set != OL_SETSEL_NONE) {
		/* only check on setting */
		if (BLI_findindex(ed->seqbasep, seq) != -1) {
			if (set == OL_SETSEL_EXTEND) {
				BKE_sequencer_active_set(scene, NULL);
			}
			ED_sequencer_deselect_all(scene);

			if ((set == OL_SETSEL_EXTEND) && seq->flag & SELECT) {
				seq->flag &= ~SELECT;
			}
			else {
				seq->flag |= SELECT;
				BKE_sequencer_active_set(scene, seq);
			}
		}

		WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER | NA_SELECTED, scene);
	}
	else {
		if (ed->act_seq == seq && seq->flag & SELECT) {
			return OL_DRAWSEL_NORMAL;
		}
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_sequence_dup(
        Scene *scene, TreeElement *te, TreeStoreElem *UNUSED(tselem), const eOLSetState set)
{
	Sequence *seq, *p;
	Editing *ed = BKE_sequencer_editing_get(scene, false);

	seq = (Sequence *)te->directdata;
	if (set == OL_SETSEL_NONE) {
		if (seq->flag & SELECT)
			return OL_DRAWSEL_NORMAL;
		return OL_DRAWSEL_NONE;
	}

// XXX	select_single_seq(seq, 1);
	p = ed->seqbasep->first;
	while (p) {
		if ((!p->strip) || (!p->strip->stripdata) || (p->strip->stripdata->name[0] == '\0')) {
			p = p->next;
			continue;
		}

//		if (STREQ(p->strip->stripdata->name, seq->strip->stripdata->name))
// XXX			select_single_seq(p, 0);
		p = p->next;
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_keymap_item(
        bContext *UNUSED(C), Scene *UNUSED(scene), SceneLayer *UNUSED(sl), TreeElement *te, TreeStoreElem *UNUSED(tselem), const eOLSetState set)
{
	wmKeyMapItem *kmi = te->directdata;
	
	if (set == OL_SETSEL_NONE) {
		if (kmi->flag & KMI_INACTIVE) {
			return OL_DRAWSEL_NONE;
		}
		return OL_DRAWSEL_NORMAL;
	}
	else {
		kmi->flag ^= KMI_INACTIVE;
	}
	return OL_DRAWSEL_NONE;
}

static eOLDrawState tree_element_active_collection(
        bContext *C, TreeElement *te, TreeStoreElem *tselem, const eOLSetState set)
{
	if (set == OL_SETSEL_NONE) {
		LayerCollection *active = CTX_data_layer_collection(C);

		/* sometimes the renderlayer has no LayerCollection at all */
		if (active == NULL) {
			return OL_DRAWSEL_NONE;
		}

		if ((tselem->type == TSE_SCENE_COLLECTION && active->scene_collection == te->directdata) ||
		    (tselem->type == TSE_LAYER_COLLECTION && active == te->directdata))
		{
			return OL_DRAWSEL_NORMAL;
		}
	}
	/* don't allow selecting a scene collection, it can have multiple layer collection
	 * instances (which one would the user want to be selected then?) */
	else if (tselem->type == TSE_LAYER_COLLECTION) {
		SceneLayer *sl = CTX_data_scene_layer(C);
		LayerCollection *lc = te->directdata;
		const int collection_index = BKE_layer_collection_findindex(sl, lc);

		BLI_assert(collection_index >= 0);
		sl->active_collection = collection_index;
		WM_main_add_notifier(NC_SCENE | ND_LAYER, NULL);
	}

	return OL_DRAWSEL_NONE;
}

/* ---------------------------------------------- */

/* generic call for ID data check or make/check active in UI */
eOLDrawState tree_element_active(bContext *C, Scene *scene, SceneLayer *sl, SpaceOops *soops, TreeElement *te,
                                 const eOLSetState set, const bool handle_all_types)
{
	switch (te->idcode) {
		/* Note: ID_OB only if handle_all_type is true, else objects are handled specially to allow multiple
		 * selection. See do_outliner_item_activate. */
		case ID_OB:
			if (handle_all_types) {
				return tree_element_set_active_object(C, scene, sl, soops, te, set, false);
			}
			break;
		case ID_MA:
			return tree_element_active_material(C, scene, sl, soops, te, set);
		case ID_WO:
			return tree_element_active_world(C, scene, sl, soops, te, set);
		case ID_LA:
			return tree_element_active_lamp(C, scene, sl, soops, te, set);
		case ID_TE:
			return tree_element_active_texture(C, scene, sl, soops, te, set);
		case ID_TXT:
			return tree_element_active_text(C, scene, sl, soops, te, set);
		case ID_CA:
			return tree_element_active_camera(C, scene, sl, soops, te, set);
	}
	return OL_DRAWSEL_NONE;
}

/**
 * Generic call for non-id data to make/check active in UI
 */
eOLDrawState tree_element_type_active(
        bContext *C, Scene *scene, SceneLayer *sl, SpaceOops *soops,
        TreeElement *te, TreeStoreElem *tselem, const eOLSetState set, bool recursive)
{
	switch (tselem->type) {
		case TSE_DEFGROUP:
			return tree_element_active_defgroup(C, sl, te, tselem, set);
		case TSE_BONE:
			return tree_element_active_bone(C, sl, te, tselem, set, recursive);
		case TSE_EBONE:
			return tree_element_active_ebone(C, scene, te, tselem, set, recursive);
		case TSE_MODIFIER:
			return tree_element_active_modifier(C, scene, sl, te, tselem, set);
		case TSE_LINKED_OB:
			if (set != OL_SETSEL_NONE) {
				tree_element_set_active_object(C, scene, sl, soops, te, set, false);
			}
			else if (tselem->id == (ID *)OBACT(sl)) {
				return OL_DRAWSEL_NORMAL;
			}
			break;
		case TSE_LINKED_PSYS:
			return tree_element_active_psys(C, scene, te, tselem, set);
		case TSE_POSE_BASE:
			return tree_element_active_pose(C, scene, sl, te, tselem, set);
		case TSE_POSE_CHANNEL:
			return tree_element_active_posechannel(C, scene, sl, te, tselem, set, recursive);
		case TSE_CONSTRAINT:
			return tree_element_active_constraint(C, scene, sl, te, tselem, set);
		case TSE_R_LAYER:
			return tree_element_active_renderlayer(C, scene, sl, te, tselem, set);
		case TSE_POSEGRP:
			return tree_element_active_posegroup(C, scene, sl, te, tselem, set);
		case TSE_SEQUENCE:
			return tree_element_active_sequence(C, scene, te, tselem, set);
		case TSE_SEQUENCE_DUP:
			return tree_element_active_sequence_dup(scene, te, tselem, set);
		case TSE_KEYMAP_ITEM:
			return tree_element_active_keymap_item(C, scene, sl, te, tselem, set);
		case TSE_GP_LAYER:
			//return tree_element_active_gplayer(C, scene, s, te, tselem, set);
			break;
		case TSE_SCENE_COLLECTION:
		case TSE_LAYER_COLLECTION:
			return tree_element_active_collection(C, te, tselem, set);
	}
	return OL_DRAWSEL_NONE;
}

/* ================================================ */

static void outliner_item_activate(
        bContext *C, SpaceOops *soops, TreeElement *te,
        const bool extend, const bool recursive)
{
	Scene *scene = CTX_data_scene(C);
	SceneLayer *sl = CTX_data_scene_layer(C);
	TreeStoreElem *tselem = TREESTORE(te);

	/* always makes active object, except for some specific types.
	 * Note about TSE_EBONE: In case of a same ID_AR datablock shared among several objects, we do not want
	 * to switch out of edit mode (see T48328 for details). */
	if (!ELEM(tselem->type, TSE_SEQUENCE, TSE_SEQ_STRIP, TSE_SEQUENCE_DUP, TSE_EBONE, TSE_LAYER_COLLECTION)) {
		tree_element_set_active_object(C, scene, sl, soops, te,
		                               (extend && tselem->type == 0) ? OL_SETSEL_EXTEND : OL_SETSEL_NORMAL,
		                               recursive && tselem->type == 0);
	}

	if (tselem->type == 0) { // the lib blocks
		/* editmode? */
		if (te->idcode == ID_SCE) {
			if (scene != (Scene *)tselem->id) {
				WM_window_change_active_scene(CTX_data_main(C), C, CTX_wm_window(C), (Scene *)tselem->id);
			}
		}
		else if (te->idcode == ID_GR) {
			Group *gr = (Group *)tselem->id;
			GroupObject *gob;
			
			if (extend) {
				int sel = BA_SELECT;
				for (gob = gr->gobject.first; gob; gob = gob->next) {
					if (gob->ob->flag & SELECT) {
						sel = BA_DESELECT;
						break;
					}
				}

				for (gob = gr->gobject.first; gob; gob = gob->next) {
					ED_object_base_select(BKE_scene_layer_base_find(sl, gob->ob), sel);
				}
			}
			else {
				BKE_scene_layer_base_deselect_all(sl);

				for (gob = gr->gobject.first; gob; gob = gob->next) {
					Base *base = BKE_scene_layer_base_find(sl, gob->ob);
					/* Object may not be in this scene */
					if (base != NULL) {
						if ((base->flag & BASE_SELECTED) == 0) {
							ED_object_base_select(base, BA_SELECT);
						}
					}
				}
			}
			
			WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
		}
		else if (ELEM(te->idcode, ID_ME, ID_CU, ID_MB, ID_LT, ID_AR)) {
			WM_operator_name_call(C, "OBJECT_OT_editmode_toggle", WM_OP_INVOKE_REGION_WIN, NULL);
		}
		else if (ELEM(te->idcode, ID_GD)) {
			/* set grease pencil to object mode */
			WM_operator_name_call(C, "GPENCIL_OT_editmode_toggle", WM_OP_INVOKE_REGION_WIN, NULL);
		}
		else {  // rest of types
			tree_element_active(C, scene, sl, soops, te, OL_SETSEL_NORMAL, false);
		}

	}
	else {
		tree_element_type_active(C, scene, sl, soops, te, tselem,
		                         extend ? OL_SETSEL_EXTEND : OL_SETSEL_NORMAL,
		                         recursive);
	}
}

/**
 * \param extend: Don't deselect other items, only modify \a te.
 * \param toggle: Select \a te when not selected, deselect when selected.
 */
static void outliner_item_select(SpaceOops *soops, const TreeElement *te, const bool extend, const bool toggle)
{
	TreeStoreElem *tselem = TREESTORE(te);
	const short new_flag = toggle ? (tselem->flag ^ TSE_SELECTED) : (tselem->flag | TSE_SELECTED);

	if (extend == false) {
		outliner_set_flag(&soops->tree, TSE_SELECTED, false);
	}
	tselem->flag = new_flag;
}

static void outliner_item_toggle_closed(TreeElement *te, const bool toggle_children)
{
	TreeStoreElem *tselem = TREESTORE(te);
	if (toggle_children) {
		tselem->flag &= ~TSE_CLOSED;

		const bool all_opened = !outliner_has_one_flag(&te->subtree, TSE_CLOSED, 1);
		outliner_set_flag(&te->subtree, TSE_CLOSED, all_opened);
	}
	else {
		tselem->flag ^= TSE_CLOSED;
	}
}

static bool outliner_item_is_co_within_close_toggle(TreeElement *te, float view_co_x)
{
	return ((te->flag & TE_ICONROW) == 0) && (view_co_x > te->xs) && (view_co_x < te->xs + UI_UNIT_X);
}

static bool outliner_is_co_within_restrict_columns(const SpaceOops *soops, const ARegion *ar, float view_co_x)
{
	return (!ELEM(soops->outlinevis, SO_DATABLOCKS, SO_USERDEF) &&
	        !(soops->flag & SO_HIDE_RESTRICTCOLS) &&
	        (view_co_x > ar->v2d.cur.xmax - OL_TOG_RESTRICT_VIEWX));
}

int outliner_item_activate_or_toggle_closed(bContext *C, int x, int y, bool extend, bool recursive)
{
	ARegion *ar = CTX_wm_region(C);
	SpaceOops *soops = CTX_wm_space_outliner(C);
	TreeElement *te;
	float view_mval[2];
	bool changed = false, rebuild_tree = false;

	UI_view2d_region_to_view(&ar->v2d, x, y, &view_mval[0], &view_mval[1]);

	if (outliner_is_co_within_restrict_columns(soops, ar, view_mval[0])) {
		return OPERATOR_CANCELLED;
	}

	if (!(te = outliner_find_item_at_y(soops, &soops->tree, view_mval[1]))) {
		/* skip */
	}
	else if (outliner_item_is_co_within_close_toggle(te, view_mval[0])) {
		outliner_item_toggle_closed(te, extend);
		changed = true;
		rebuild_tree = true;
	}
	else {
		/* the row may also contain children, if one is hovered we want this instead of current te */
		TreeElement *activate_te = outliner_find_item_at_x_in_row(soops, te, view_mval[0]);

		outliner_item_select(soops, activate_te, extend, extend);
		outliner_item_activate(C, soops, activate_te, extend, recursive);
		changed = true;
	}

	if (changed) {
		if (!rebuild_tree) {
			/* only needs to redraw, no rebuild */
			soops->storeflag |= SO_TREESTORE_REDRAW;
		}
		ED_undo_push(C, "Outliner selection change");
		ED_region_tag_redraw(ar);
	}

	return OPERATOR_FINISHED;
}

/* event can enterkey, then it opens/closes */
static int outliner_item_activate_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	bool extend    = RNA_boolean_get(op->ptr, "extend");
	bool recursive = RNA_boolean_get(op->ptr, "recursive");
	int x = event->mval[0];
	int y = event->mval[1];
	return outliner_item_activate_or_toggle_closed(C, x, y, extend, recursive);
}

void OUTLINER_OT_item_activate(wmOperatorType *ot)
{
	ot->name = "Activate Item";
	ot->idname = "OUTLINER_OT_item_activate";
	ot->description = "Handle mouse clicks to activate/select items";
	
	ot->invoke = outliner_item_activate_invoke;
	
	ot->poll = ED_operator_outliner_active;
	
	RNA_def_boolean(ot->srna, "extend", true, "Extend", "Extend selection for activation");
	RNA_def_boolean(ot->srna, "recursive", false, "Recursive", "Select Objects and their children");
}

/* ****************************************************** */

/* **************** Border Select Tool ****************** */
static void outliner_item_border_select(Scene *scene, rctf *rectf, TreeElement *te, bool select)
{
	TreeStoreElem *tselem = TREESTORE(te);

	if (te->ys <= rectf->ymax && te->ys + UI_UNIT_Y >= rectf->ymin) {
		if (select) {
			tselem->flag |= TSE_SELECTED;
		}
		else {
			tselem->flag &= ~TSE_SELECTED;
		}
	}

	/* Look at its children. */
	if ((tselem->flag & TSE_CLOSED) == 0) {
		for (te = te->subtree.first; te; te = te->next) {
			outliner_item_border_select(scene, rectf, te, select);
		}
	}
}

static int outliner_border_select_exec(bContext *C, wmOperator *op)
{
	Scene *scene = CTX_data_scene(C);
	SpaceOops *soops = CTX_wm_space_outliner(C);
	ARegion *ar = CTX_wm_region(C);
	TreeElement *te;
	rctf rectf;
	bool select = !RNA_boolean_get(op->ptr, "deselect");

	WM_operator_properties_border_to_rctf(op, &rectf);
	UI_view2d_region_to_view_rctf(&ar->v2d, &rectf, &rectf);

	for (te = soops->tree.first; te; te = te->next) {
		outliner_item_border_select(scene, &rectf, te, select);
	}

	WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
	ED_region_tag_redraw(ar);

	return OPERATOR_FINISHED;
}

void OUTLINER_OT_select_border(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Border Select";
	ot->idname = "OUTLINER_OT_select_border";
	ot->description = "Use box selection to select tree elements";

	/* api callbacks */
	ot->invoke = WM_gesture_border_invoke;
	ot->exec = outliner_border_select_exec;
	ot->modal = WM_gesture_border_modal;
	ot->cancel = WM_gesture_border_cancel;

	ot->poll = ED_operator_outliner_active;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

	/* rna */
	WM_operator_properties_gesture_border_ex(ot, true, false);
}

/* ****************************************************** */
