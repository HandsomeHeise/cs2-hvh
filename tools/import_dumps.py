"""Generate the small, versioned offset profile used by local-player diagnostics."""
import argparse
import hashlib
import json
from pathlib import Path
from inspect_pe import PE

ROOT = Path(__file__).resolve().parents[1]
FIELDS = {
    "health": ("C_BaseEntity", "m_iHealth"),
    "team": ("C_BaseEntity", "m_iTeamNum"),
    "life_state": ("C_BaseEntity", "m_lifeState"),
    "identity": ("CEntityInstance", "m_pEntity"),
    "designer_name": ("CEntityIdentity", "m_designerName"),
    "pawn_handle": ("CCSPlayerController", "m_hPlayerPawn"),
    "player_name": ("CBasePlayerController", "m_iszPlayerName"),
    "weapon_services": ("C_BasePlayerPawn", "m_pWeaponServices"),
    "active_weapon": ("CPlayer_WeaponServices", "m_hActiveWeapon"),
    "clip": ("C_BasePlayerWeapon", "m_iClip1"),
    "reserve_ammo": ("C_BasePlayerWeapon", "m_pReserveAmmo"),
    "max_clip": ("CBasePlayerWeaponVData", "m_iMaxClip1"),
    "armor": ("C_CSPlayerPawn", "m_ArmorValue"),
    "defusing": ("C_CSPlayerPawn", "m_bIsDefusing"),
    "item_services": ("C_BasePlayerPawn", "m_pItemServices"),
    "has_helmet": ("CCSPlayer_ItemServices", "m_bHasHelmet"),
    "has_defuser": ("CCSPlayer_ItemServices", "m_bHasDefuser"),
    "attribute_manager": ("C_EconEntity", "m_AttributeManager"),
    "item": ("C_AttributeContainer", "m_Item"),
    "item_definition": ("C_EconItemView", "m_iItemDefinitionIndex"),
    "weapons": ("CPlayer_WeaponServices", "m_hMyWeapons"),
    "owner_handle": ("C_BaseEntity", "m_hOwnerEntity"),
    "item_id_high": ("C_EconItemView", "m_iItemIDHigh"),
    "item_quality": ("C_EconItemView", "m_iEntityQuality"),
    "item_initialized": ("C_EconItemView", "m_bInitialized"),
    "item_disallow_soc": ("C_EconItemView", "m_bDisallowSOC"),
    "attribute_list": ("C_EconItemView", "m_AttributeList"),
    "attribute_vector": ("CAttributeList", "m_Attributes"),
    "attribute_definition": ("CEconItemAttribute", "m_iAttributeDefinitionIndex"),
    "attribute_value": ("CEconItemAttribute", "m_flValue"),
    "fallback_paint": ("C_EconEntity", "m_nFallbackPaintKit"),
    "fallback_seed": ("C_EconEntity", "m_nFallbackSeed"),
    "fallback_wear": ("C_EconEntity", "m_flFallbackWear"),
    "fallback_stattrak": ("C_EconEntity", "m_nFallbackStatTrak"),
    "gloves": ("C_CSPlayerPawn", "m_EconGloves"),
    "reapply_gloves": ("C_CSPlayerPawn", "m_bNeedToReApplyGloves"),
    "gloves_changed": ("C_CSPlayerPawn", "m_nEconGlovesChanged"),
    "scene_node": ("C_BaseEntity", "m_pGameSceneNode"),
    "scene_owner": ("CGameSceneNode", "m_pOwner"),
    "scene_child": ("CGameSceneNode", "m_pChild"),
    "scene_sibling": ("CGameSceneNode", "m_pNextSibling"),
    "scene_parent": ("CGameSceneNode", "m_pParent"),
    "hud_arms": ("C_CSPlayerPawn", "m_hHudModelArms"),
    "model_state": ("CSkeletonInstance", "m_modelState"),
    "model_name": ("CModelState", "m_ModelName"),
    "model_mesh_mask": ("CModelState", "m_MeshGroupMask"),
    "model_body_choices": ("CModelState", "m_nBodyGroupChoices"),
    "viewmodel_attachment": ("C_EconEntity", "m_hViewmodelAttachment"),
    "view_offset": ("C_BaseModelEntity", "m_vecViewOffset"),
    "abs_origin": ("CGameSceneNode", "m_vecAbsOrigin"),
    "dormant": ("CGameSceneNode", "m_bDormant"),
    "flags": ("C_BaseEntity", "m_fFlags"),
    "velocity": ("C_BaseEntity", "m_vecVelocity"),
    "immunity": ("C_CSPlayerPawn", "m_bGunGameImmunity"),
    "flash_duration": ("C_CSPlayerPawnBase", "m_flFlashDuration"),
    "scoped": ("C_CSPlayerPawn", "m_bIsScoped"),
    "reloading": ("C_CSWeaponBase", "m_bInReload"),
    "aim_punch_services": ("C_CSPlayerPawn", "m_pAimPunchServices"),
}
GLOBALS = {
    "local_controller": ("client.dll", "dwLocalPlayerController"),
    "local_pawn": ("client.dll", "dwLocalPlayerPawn"),
    "entity_system": ("client.dll", "dwGameEntitySystem"),
    "view_matrix": ("client.dll", "dwViewMatrix"),
    "csgo_input": ("client.dll", "dwCSGOInput"),
    "build_number": ("engine2.dll", "dwBuildNumber"),
    "network_client": ("engine2.dll", "dwNetworkGameClient"),
    "sign_on": ("engine2.dll", "dwNetworkGameClient_signOnState"),
    "background_map": ("engine2.dll", "dwNetworkGameClient_isBackgroundMap"),
    "max_clients": ("engine2.dll", "dwNetworkGameClient_maxClients"),
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--game", type=Path, required=True)
    args = parser.parse_args()
    paths = {name: args.source / name for name in ("info.json", "offsets.json", "client_dll.json")}
    content = {name: json.loads(path.read_text(encoding="utf-8")) for name, path in paths.items()}
    classes = content["client_dll.json"]["client.dll"]["classes"]
    values = {name: classes[cls]["fields"][field] for name, (cls, field) in FIELDS.items()}
    values.update({name: content["offsets.json"][dll][field] for name, (dll, field) in GLOBALS.items()})
    values["expected_build"] = content["info.json"]["build_number"]
    images = {}
    for label, relative in [("client", "csgo/bin/win64/client.dll"), ("engine", "bin/win64/engine2.dll")]:
        path = args.game / relative
        image = PE(path)
        pe_offset = image.u32(0x3c)
        values[label + "_timestamp"] = image.u32(pe_offset + 8)
        values[label + "_image_size"] = image.u32(pe_offset + 24 + 56)
        images[label] = {"path": str(path), "sha256": hashlib.sha256(image.data).hexdigest()}
    lines = ["#pragma once", "#include <cstdint>", "// Generated by tools/import_dumps.py. Do not hand-edit schema offsets.", "namespace dump_offsets {"]
    lines += [f"inline constexpr std::uint32_t {name} = 0x{value:x};" for name, value in values.items()]
    lines += ["}", ""]
    generated = ROOT / "src/generated/dump_offsets.hpp"
    generated.parent.mkdir(parents=True, exist_ok=True)
    generated.write_text("\n".join(lines), encoding="utf-8")
    report = {"source": str(args.source), "info": content["info.json"], "values": values, "images": images,
              "dump_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}}
    (ROOT / "analysis/dump-profile.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Generated", generated, "for build", values["expected_build"])

if __name__ == "__main__":
    main()
