import bpy
path = r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_anim.glb'
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
scene = bpy.context.scene
for name in ['gear_c','wheel_c','aileron_l','flap_r','prop01_1']:
    obj = scene.objects.get(name)
    if obj:
        print(name, 'parent=', obj.parent.name if obj.parent else None, 'type=', obj.type)
        cur = obj.parent
        while cur:
            print('  chain', cur.name, cur.type, tuple(round(v,6) for v in cur.scale), round(cur.matrix_world.to_3x3().determinant(),6))
            cur = cur.parent
