import bpy
from mathutils import Vector

paths = {
    'rig': r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_rig.glb',
    'anim': r'C:\Users\AldertLake\Documents\Random Projects\Dagor_Assets_Extractor\scratch\dummy_plane_cockpit_anim.glb',
}

def load(path, frame=None):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=path)
    scene = bpy.context.scene
    if frame is not None:
        scene.frame_set(frame)
    result = {}
    for obj in scene.objects:
        if obj.type != 'MESH':
            continue
        world_verts = [obj.matrix_world @ v.co for v in obj.data.vertices]
        if not world_verts:
            continue
        center = sum(world_verts, Vector((0,0,0))) / len(world_verts)
        mins = Vector((min(v.x for v in world_verts), min(v.y for v in world_verts), min(v.z for v in world_verts)))
        maxs = Vector((max(v.x for v in world_verts), max(v.y for v in world_verts), max(v.z for v in world_verts)))
        result[obj.name] = {'center': center, 'mins': mins, 'maxs': maxs}
    return result

rig = load(paths['rig'], frame=1)
anim = load(paths['anim'], frame=1)
for name in sorted(set(rig) & set(anim)):
    cdelta = (anim[name]['center'] - rig[name]['center']).length
    mindelta = (anim[name]['mins'] - rig[name]['mins']).length
    maxdelta = (anim[name]['maxs'] - rig[name]['maxs']).length
    if cdelta > 1e-4 or mindelta > 1e-4 or maxdelta > 1e-4:
        print(name, 'centerDelta', round(cdelta,6), 'minDelta', round(mindelta,6), 'maxDelta', round(maxdelta,6), 'rigCenter', tuple(round(v,6) for v in rig[name]['center']), 'animCenter', tuple(round(v,6) for v in anim[name]['center']))
