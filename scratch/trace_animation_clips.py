import struct
import json

def trace():
    glb_path = "C:/Users/AldertLake/Documents/DagorAssetsExtractorExports/f_16a_block_10_norway_cockpit/f_16a_block_10_norway_cockpit.glb"
    with open(glb_path, 'rb') as f:
        data = f.read()
    
    l, t = struct.unpack('<II', data[12:20])
    js = json.loads(data[20:20+l].decode('utf-8'))
    
    buffer_views = js['bufferViews']
    accessors = js['accessors']
    animations = js['animations']
    
    # Range of regular curves of anim 11: offset from 189797772 to 206212292
    target_bvs = {}
    for idx, bv in enumerate(buffer_views):
        if 189797772 <= bv['byteOffset'] < 206212292:
            target_bvs[idx] = bv
            
    print(f"Found {len(target_bvs)} bufferViews in range.")
    
    # Find accessors referencing these bufferViews
    target_accs = {}
    for idx, acc in enumerate(accessors):
        bv_idx = acc.get('bufferView')
        if bv_idx in target_bvs:
            target_accs[idx] = (bv_idx, acc)
            
    print(f"Found {len(target_accs)} accessors referencing those bufferViews.")
    
    # Find which animations and samplers reference these accessors
    for anim_idx, anim in enumerate(animations):
        samplers = anim.get('samplers', [])
        found_count = 0
        for s_idx, sampler in enumerate(samplers):
            in_acc = sampler['input']
            out_acc = sampler['output']
            if in_acc in target_accs or out_acc in target_accs:
                found_count += 1
        if found_count > 0:
            print(f"Animation {anim_idx} ({anim.get('name')}) has {found_count} samplers referencing these accessors.")

if __name__ == '__main__':
    trace()
