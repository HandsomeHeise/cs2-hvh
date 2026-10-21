"""Read selected cosmetic fields and interface addresses; never writes to CS2."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--client', type=int, required=True)
    p.add_argument('--output', type=Path, default=ROOT / 'analysis/cosmetic-state.json')
    args = p.parse_args()
    k = c.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [w.DWORD,w.BOOL,w.DWORD]; k.OpenProcess.restype=w.HANDLE
    k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
    k.CloseHandle.argtypes=[w.HANDLE]
    process=k.OpenProcess(0x1010,False,args.pid)
    if not process: raise OSError(c.get_last_error(),'OpenProcess')
    def raw(a,n):
        b=c.create_string_buffer(n); got=c.c_size_t()
        return b.raw if a and k.ReadProcessMemory(process,a,b,n,c.byref(got)) and got.value==n else None
    def value(a,fmt='<Q'):
        b=raw(a,struct.calcsize(fmt)); return struct.unpack(fmt,b)[0] if b else None
    def text(a):
        b=raw(a,128); return b.split(b'\0',1)[0].decode('utf-8','replace') if b else None
    profile=json.loads((ROOT/'analysis/dump-profile.json').read_text())
    classes=json.loads((Path(profile['source'])/'client_dll.json').read_text())['client.dll']['classes']
    interfaces=json.loads((Path(profile['source'])/'interfaces.json').read_text())['client.dll']
    o=profile['values']; base=args.client
    def field(cls,name): return classes[cls]['fields'][name]
    def econ(item):
        fields={name:value(item+field('C_EconItemView',name),fmt) for name,fmt in
                [('m_iItemDefinitionIndex','<H'),('m_iItemIDHigh','<I'),('m_bInitialized','<B'),('m_bDisallowSOC','<B')]}
        # Never include account IDs or player identities in this report.
        for name in ['m_AttributeList','m_NetworkedDynamicAttributes']:
            address=item+field('C_EconItemView',name)+field('CAttributeList','m_Attributes')
            count=value(address,'<i'); data=value(address+8)
            entries=[]
            if count is not None and 0<=count<=48 and (not count or data):
                for index in range(count):
                    entry=data+index*0x48
                    entries.append({'definition':value(entry+field('CEconItemAttribute','m_iAttributeDefinitionIndex'),'<H'),
                                    'value':value(entry+field('CEconItemAttribute','m_flValue'),'<f'),
                                    'value_bits':value(entry+field('CEconItemAttribute','m_flValue'),'<I')})
            fields[name]={'bytes':raw(address,24).hex() if raw(address,24) else None,'count':count,'entries':entries}
        return fields
    def render_materials(instance):
        # Bounded reads following native93afd0 /1c9ce0 /3b9bf0; no virtual calls.
        component=value(instance+0x338)
        count=value(component+0x38,'<i') if component else None
        handles=value(component+0x40) if component else 0
        result={'object_count':count,'objects':[]}
        if count is None or not 0<=count<=32 or not handles:return result
        for index in range(count):
            handle=value(handles+8*index)
            descriptor=value(handle+0x20) if handle else 0
            flags=value(descriptor+0x78) if descriptor else None
            obj=value(descriptor+0x10) if descriptor else 0
            entry={'flags':hex(flags) if flags is not None else None,'materials':[]}
            for _ in range(8):
                if not obj:break
                mesh=value(obj+0x20) or value(obj+0x18)
                draw_count=value(mesh+0x18,'<I') if mesh else None
                allocated=value(mesh+0x1c,'<I') if mesh else 0
                draws=value(mesh+0x20) if mesh and allocated and allocated&0x7fffffff else 0
                if draws and draw_count is not None and 0<=draw_count<=64:
                    for draw in range(draw_count):
                        binding=value(draws+draw*0x120+0x118)
                        material=value(binding) if binding else 0
                        table=value(material) if material else 0
                        getter=value(table+0xe0) if table else 0
                        original=value(material+0x60) if getter and raw(getter,11)==bytes.fromhex('488b4160488902488bc2c3') else 0
                        entry['materials'].append({'binding':hex(binding) if binding else None,
                            'name':text(value(material+0x10)) if material else None,
                            'source_binding':hex(original) if original else None})
                obj=value(obj+0x58)
            result['objects'].append(entry)
        return result
    try:
        iface=base+interfaces['Source2Client002']; table=value(iface)
        if raw(base,2) != b'MZ' or not table or not base <= table < base + o['client_image_size']:
            raise SystemExit('Selected process/client base is unavailable; no report was written.')
        result={'pid':args.pid,'method':'ReadProcessMemory only; no target calls or writes',
                'client_interface_rva':hex(interfaces['Source2Client002']),
                'client_vtable_rva':hex(table-base) if table else None,
                'client_methods':[{ 'slot':i,'va':hex(0x180000000+fn-base),'rva':hex(fn-base)} for i in range(80) if table and (fn:=value(table+i*8)) and base<=fn<base+o['client_image_size']]}
        pawn=value(base+o['local_pawn']); system=value(base+o['entity_system'])
        def entity(handle):
            if handle is None or handle==0xffffffff:return 0
            page=value(system+16+8*((handle&0x7fff)//512))
            identity=page+(handle&511)*0x70 if page else 0
            instance=value(identity) if identity else 0
            return instance if instance and value(identity+16,'<I')==handle and value(instance+o['identity'])==identity else 0
        result['weapons']=[]
        if pawn:
            def scene_tree(root_node):
                queue=[(root_node,0)]; seen=set(); nodes=[]
                while queue and len(seen)<64:
                    node,depth=queue.pop(0)
                    if not node or node in seen:continue
                    seen.add(node)
                    owner=value(node+field('CGameSceneNode','m_pOwner'))
                    identity=value(owner+o['identity']) if owner else 0
                    model=text(value(node+o['model_state']+o['model_name']))
                    nodes.append({'depth':depth,'node':hex(node),'owner':hex(owner) if owner else None,
                        'entity_vtable_rva':hex(value(owner)-base) if owner and value(owner) else None,
                        'skeleton_vtable_rva':hex(value(node)-base) if value(node) else None,
                        'entity_handle':hex(value(identity+16,'<I')) if identity and value(identity+16,'<I') is not None else None,
                        'owner_handle':hex(value(owner+o['owner_handle'],'<I')) if owner and value(owner+o['owner_handle'],'<I') is not None else None,
                        'designer':text(value(identity+o['designer_name'])) if identity else None,
                        'model':model,'mesh_mask':value(node+o['model_state']+o['model_mesh_mask']),
                        'render':render_materials(owner) if owner and model and model.startswith(('weapons/','models/')) else None})
                    sibling=value(node+field('CGameSceneNode','m_pNextSibling'))
                    if sibling:queue.append((sibling,depth))
                    child=value(node+field('CGameSceneNode','m_pChild'))
                    if child and depth<4:queue.append((child,depth+1))
                return nodes
            arms_handle=value(pawn+field('C_CSPlayerPawn','m_hHudModelArms'),'<I')
            arms=entity(arms_handle)
            result['hud_arms']={'handle':hex(arms_handle) if arms_handle is not None else None,
                'tree':scene_tree(value(arms+o['scene_node'])) if arms else []}
            result['local_scene_tree']=scene_tree(value(pawn+o['scene_node']))
            result['gloves']=econ(pawn+field('C_CSPlayerPawn','m_EconGloves'))
            result['glove_flags']={name:value(pawn+field('C_CSPlayerPawn',name),'<B') for name in ['m_bNeedToReApplyGloves','m_nEconGlovesChanged']}
            service=value(pawn+o['weapon_services'])
            vector=service+field('CPlayer_WeaponServices','m_hMyWeapons') if service else 0
            result['weapon_vector_bytes']=raw(vector,24).hex() if vector and raw(vector,24) else None
            # Source2 CNetworkUtlVectorBase: int32 count at 0, pointer at 8.
            count=value(vector,'<i'); data=value(vector+8)
            if count is not None and 0<=count<=64 and data:
                for i in range(count):
                    handle=value(data+4*i,'<I'); instance=entity(handle)
                    if not instance:continue
                    item=instance+o['attribute_manager']+o['item']
                    identity=value(instance+o['identity']); designer=value(identity+o['designer_name'])
                    node=value(instance+field('C_BaseEntity','m_pGameSceneNode'))
                    model=text(value(node+field('CSkeletonInstance','m_modelState')+field('CModelState','m_ModelName'))) if node else None
                    attachment_handle=value(instance+field('C_EconEntity','m_hViewmodelAttachment'),'<I')
                    attachment=entity(attachment_handle)
                    attachment_node=value(attachment+o['scene_node']) if attachment else 0
                    choices=node+o['model_state']+field('CModelState','m_nBodyGroupChoices') if node else 0
                    choices_count=value(choices,'<i') if choices else None
                    choices_data=value(choices+8) if choices else 0
                    generated=[]
                    material_count=value(instance+0xab0,'<i'); material_data=value(instance+0xab8)
                    if material_data and material_count is not None and 0<=material_count<=16:
                        for material_index in range(material_count):
                            binding=value(material_data+material_index*8)
                            material=value(binding) if binding else 0
                            material_table=value(material) if material else 0
                            getter=value(material_table+0xe0) if material_table else 0
                            original_binding=value(material+0x60) if getter and raw(getter,11)==bytes.fromhex('488b4160488902488bc2c3') else 0
                            original_material=value(original_binding) if original_binding else 0
                            original_table=value(original_material) if original_material else 0
                            generated.append({'material':hex(material) if material else None,'table':hex(material_table) if material_table else None,
                                'original_getter':hex(getter) if getter else None,'getter_bytes':raw(getter,32).hex() if getter and raw(getter,32) else None,
                                'original_binding':hex(original_binding) if original_binding else None,'original_material':hex(original_material) if original_material else None,
                                'original_name':text(value(original_material+0x10)) if original_material else None,
                                'generated_name':text(value(material+0x10)) if material else None,
                                'original_methods':[{'slot':slot,'method':hex(fn),'bytes':raw(fn,24).hex() if raw(fn,24) else None} for slot in range(6) if original_table and (fn:=value(original_table+8*slot))]})
                    result['weapons'].append({'handle':hex(handle),'designer':text(designer),'econ':econ(item),
                        'model':model,
                        'render':render_materials(instance),
                        'mesh_mask':value(node+o['model_state']+o['model_mesh_mask']) if node else None,
                        'skeleton_vtable_rva':hex(value(node)-base) if node and value(node) else None,
                        'skeleton_owner_matches':value(node+o['scene_owner'])==instance if node else False,
                        'body_choices':{'count':choices_count,'values':[value(choices_data+4*j,'<I') for j in range(choices_count)] if choices_data and choices_count is not None and 0<=choices_count<=64 else []},
                        'viewmodel_attachment':{'handle':hex(attachment_handle) if attachment_handle is not None else None,
                            'resolved':bool(attachment),
                            'mesh_mask':value(attachment_node+o['model_state']+o['model_mesh_mask']) if attachment_node else None,
                            'model':text(value(attachment_node+o['model_state']+o['model_name'])) if attachment_node else None},
                        # Exact-build native owner offsets, corroborated in cosmetic-material-fix-recovery.json.
                        'materials':{'active_count':value(instance+0xab0,'<i'),'pending_count':value(instance+0xac8,'<i'),
                            'descriptor_mesh_mask':value(instance+0x1a88),'generated':generated},
                        'fallback':{name:value(instance+field('C_EconEntity',name),fmt) for name,fmt in
                        [('m_nFallbackPaintKit','<i'),('m_nFallbackSeed','<i'),('m_flFallbackWear','<f'),('m_nFallbackStatTrak','<i')]},
                        'vtable_rva':hex(value(instance)-base)})
        args.output.write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2))
    finally:k.CloseHandle(process)

if __name__=='__main__': main()
