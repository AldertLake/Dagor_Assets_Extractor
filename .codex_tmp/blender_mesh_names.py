import bpy
path = r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_anim.glb'
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
scene = bpy.context.scene
scene.frame_set(1)
for obj in scene.objects:
    if obj.type == 'MESH':
        print(obj.name, 'parent=', obj.parent.name if obj.parent else None, 'scale=', tuple(round(v,6) for v in obj.scale), 'det=', round(obj.matrix_world.to_3x3().determinant(),6), 'loc=', tuple(round(v,6) for v in obj.matrix_world.translation))
