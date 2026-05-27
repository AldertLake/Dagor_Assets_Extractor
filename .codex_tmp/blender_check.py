import bpy
from mathutils import Vector

paths = {
    'norig': r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_norig.glb',
    'rig': r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_rig.glb',
    'anim': r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_anim.glb',
}

def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)

def import_file(path):
    bpy.ops.import_scene.gltf(filepath=path)
    scene = bpy.context.scene
    data = {}
    meshes = [o for o in scene.objects if o.type == 'MESH']
    armatures = [o for o in scene.objects if o.type == 'ARMATURE']
    empties = [o for o in scene.objects if o.type == 'EMPTY']
    print(f'FILE {path}')
    print(f'  objects={len(scene.objects)} meshes={len(meshes)} armatures={len(armatures)} empties={len(empties)}')
    bad_scale = []
    bad_det = []
    for obj in meshes:
        det = obj.matrix_world.to_3x3().determinant()
        scl = tuple(round(v, 6) for v in obj.scale)
        loc = tuple(round(v, 6) for v in obj.location)
        data[obj.name] = {
            'loc': Vector(obj.matrix_world.translation),
            'det': det,
            'scale': scl,
        }
        if any(abs(abs(v) - 1.0) > 1e-4 for v in obj.scale):
            bad_scale.append((obj.name, scl, loc))
        if det < 0.0:
            bad_det.append((obj.name, round(det, 6), scl, loc))
    print(f'  mesh_negative_det={len(bad_det)} mesh_non_unit_scale={len(bad_scale)}')
    for row in bad_det[:12]:
        print('    NEGDET', row)
    for row in bad_scale[:12]:
        print('    SCALE', row)
    return data

results = {}
for key, path in paths.items():
    clear_scene()
    bpy.context.scene.frame_set(0)
    results[key] = import_file(path)

print('COMPARE rig vs norig')
for name in sorted(set(results['rig']) & set(results['norig'])):
    delta = (results['rig'][name]['loc'] - results['norig'][name]['loc']).length
    if delta > 1e-4:
        print('  DIFF', name, round(delta, 6), tuple(round(v, 6) for v in results['rig'][name]['loc']), tuple(round(v, 6) for v in results['norig'][name]['loc']))

clear_scene()
bpy.ops.import_scene.gltf(filepath=paths['anim'])
scene = bpy.context.scene
print(f'ANIM RANGE start={scene.frame_start} end={scene.frame_end}')
anim_data = {}
for frame in [scene.frame_start, min(scene.frame_start + 1, scene.frame_end), scene.frame_end]:
    scene.frame_set(frame)
    print(f'FRAME {frame}')
    for name in ['gear_c', 'wheel_c', 'prop01_1', 'flap_r', 'aileron_l']:
        obj = scene.objects.get(name)
        if obj is None:
            continue
        loc = tuple(round(v, 6) for v in obj.matrix_world.translation)
        det = round(obj.matrix_world.to_3x3().determinant(), 6)
        print('  ', name, 'loc', loc, 'det', det, 'scale', tuple(round(v, 6) for v in obj.scale))
