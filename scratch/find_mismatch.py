import struct
import json
import sys

def find_mismatch(glb_path):
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    # Parse GLB chunks
    offset = 12
    json_data = None
    while offset < len(data):
        chunk_len, chunk_type = struct.unpack('<II', data[offset:offset+8])
        chunk_data = data[offset+8:offset+8+chunk_len]
        if chunk_type == 0x4E4F534A: # "JSON"
            json_data = json.loads(chunk_data.decode('utf-8'))
            break
        offset += 8 + chunk_len
        
    if not json_data:
        print("No JSON found.")
        return

    meshes = json_data.get('meshes', [])
    accessors = json_data.get('accessors', [])
    buffer_views = json_data.get('bufferViews', [])
    skins = json_data.get('skins', [])
    animations = json_data.get('animations', [])

    print(f"Num meshes: {len(meshes)}")
    print(f"Num accessors: {len(accessors)}")
    print(f"Num bufferViews: {len(buffer_views)}")
    print(f"Num skins: {len(skins)}")
    print(f"Num animations: {len(animations)}")

    # Let's count how many primitives we have across all meshes
    total_mesh_prims = sum(len(m.get('primitives', [])) for m in meshes)
    print(f"Total primitives across all meshes: {total_mesh_prims}")

    # Let's count how many animation samplers/channels we have
    total_anim_samplers = sum(len(anim.get('samplers', [])) for anim in animations)
    total_anim_channels = sum(len(anim.get('channels', [])) for anim in animations)
    print(f"Total anim samplers: {total_anim_samplers}, channels: {total_anim_channels}")

    # Let's check the first accessor that fails
    # E.g. Accessor 43485
    # Let's print details of accessors around 43485
    failed_indices = [43485, 43489]
    for idx in failed_indices:
        if idx < len(accessors):
            acc = accessors[idx]
            bv_idx = acc.get('bufferView')
            print(f"\nAccessor {idx}: {acc}")
            if bv_idx is not None and bv_idx < len(buffer_views):
                print(f"  Referenced bufferView {bv_idx}: {buffer_views[bv_idx]}")

    # Let's trace how many accessors are written for meshes
    # A mesh's primitives refer to attributes (POSITION, NORMAL, etc.) and indices
    mesh_attribute_accessors_count = 0
    mesh_index_accessors_count = 0
    for idx, m in enumerate(meshes):
        for p_idx, p in enumerate(m.get('primitives', [])):
            attrs = p.get('attributes', {})
            mesh_index_accessors_count += 1
            mesh_attribute_accessors_count = max(mesh_attribute_accessors_count, *attrs.values())

    print(f"\nMax mesh attribute accessor index: {mesh_attribute_accessors_count}")
    print(f"Mesh index accessors count: {mesh_index_accessors_count}")

    # Let's check if the number of allocated index accessors in the C++ code matches mesh_index_accessors_count.
    # In C++, the allocation loop for index accessors is:
    # for each mesh, if primCount == 0 primCount = 1, allocate accessor index for each primitive.
    # But wait, in the C++ writing code:
    # for each mesh, for each primitive in mesh.primitives, write accessor.
    # Let's check if there are any meshes with 0 primitives.
    meshes_with_zero_primitives = [idx for idx, m in enumerate(meshes) if len(m.get('primitives', [])) == 0]
    print(f"Meshes with zero primitives: {meshes_with_zero_primitives}")

if __name__ == '__main__':
    find_mismatch("C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb")
