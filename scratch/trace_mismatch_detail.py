import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
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

    buffer_views = json_data.get('bufferViews', [])
    accessors = json_data.get('accessors', [])
    animations = json_data.get('animations', [])
    nodes = json_data.get('nodes', [])

    target_acc_idx = 43857
    print(f"Accessor {target_acc_idx}: {accessors[target_acc_idx]}")
    bv_idx = accessors[target_acc_idx]['bufferView']
    print(f"Referenced BufferView {bv_idx}: {buffer_views[bv_idx]}")

    # Find the animation and sampler/channel referencing this accessor
    for anim_idx, anim in enumerate(animations):
        for sampler_idx, sampler in enumerate(anim.get('samplers', [])):
            if sampler['output'] == target_acc_idx or sampler['input'] == target_acc_idx:
                print(f"\nFound in Anim {anim_idx} ({anim.get('name')}), Sampler {sampler_idx}: {sampler}")
                # Find channel targeting this sampler
                for ch_idx, ch in enumerate(anim.get('channels', [])):
                    if ch['sampler'] == sampler_idx:
                        node_name = nodes[ch['target']['node']].get('name', 'unknown')
                        print(f"  Referenced by Channel {ch_idx}: target path '{ch['target']['path']}' on node {ch['target']['node']} ({node_name})")

if __name__ == '__main__':
    trace()
